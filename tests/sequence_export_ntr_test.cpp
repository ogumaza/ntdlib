// SPDX-License-Identifier: MIT

// Tests of the conversion of the DS's sound archives (ntdlib/sequence_export/ntr), on archives that sdat::BuildArchive
// lays out and on a cartridge image that testing/nds_files.h builds. The expected values are worked out by hand from
// the sound unit and the driver: the mixer's linear pan, the timers' rate, the PSG's duties, the noise's shift
// register, the driver's tempo, its rates of attack, decay and release, and its table of levels.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/ntr/archive_input.h"
#include "ntdlib/sequence_export/ntr/convert.h"
#include "ntdlib/sequence_export/ntr/instruments.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/nds_files.h"
#include "ntdlib/testing/sdat_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "temp_folder.h"

namespace
{

namespace dse_song_files = ntdlib::testing::dse_song_files;
namespace files = ntdlib::testing::sdat_files;
namespace fs = std::filesystem;
namespace nds_files = ntdlib::testing::nds_files;
namespace sdat = ntdlib::sdat;
using namespace ntdlib::sequence_export::ntr;
using files::Bytes;
using seqlib::midi::EventKind;
using xsflib::testing::Check;

constexpr uint16_t kNone = 0xFFFF;

// The name that the conversions' files give the program.
constexpr char kProgram[] = "xyz2midi";

// A frame of the driver in milliseconds: 2728 * 64 cycles of the system clock of 33,513,982 Hz.
constexpr double kFrameMs = 1000.0 * 2728 * 64 / 33513982.0;

// The bank's instruments, by program:
//   0: wave 0, a PCM8 square that loops
//   1: the PSG at duty 3, at root key 69
//   2: the noise
//   3: a drum set from key 36: wave 0, the PSG at duty 1 and the noise
//   4: a key split up to keys 48, 40 and 127: wave 0, wave 1 and the PSG at duty 0. The second region never plays.
//   5: a wave at an address that the game's code sets
//   6: none
//   7: wave 1 with an attack of 100, a decay of 30, a sustain of 64, a release of 0xFF and a pan of 0
Bytes TestBank()
{
    return files::Bank({
        {.type = 1, .region = {.wave = 0}},
        {.type = 2, .region = {.wave = 3, .root_key = 69}},
        {.type = 3},
        {.type = 16, .min_key = 36, .regions = {{.wave = 0, .root_key = 36}, {.type = 2, .wave = 1}, {.type = 3}}},
        {.type = 17, .split_keys = {48, 40, 127}, .regions = {{.wave = 0}, {.wave = 1}, {.type = 2, .wave = 0}}},
        {.type = 4},
        {},
        {.type = 1, .region = {.wave = 1, .attack = 100, .decay = 30, .sustain = 64, .release = 0xFF, .pan = 0}},
    });
}

// The waves: 0, a PCM8 square at about 32,768 Hz that loops from its start, and 1, eight PCM16 samples at about
// 16,384 Hz that play once. A wave's length is its loop's start and length, in 32-bit words.
Bytes TestWaveArchive()
{
    const files::Wave square{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    files::Wave ramp{.encoding = 1, .sample_rate = 16384, .loop_length = 4};
    for (int i = 0; i < 8; i++)
    {
        files::Append16(ramp.data, static_cast<uint32_t>(i * 0x1000));
    }

    return files::WaveArchive({square, ramp});
}

// An archive whose sequence 0, `name`, plays `bytecode` on `bank`, on player 0, at volume `volume`. The player may
// take the channels of `channels`, or any channel for 0.
Bytes TestArchiveBytes(const Bytes& bytecode, const std::string& name = "BGM_TEST", uint16_t channels = 0,
                       uint8_t volume = 127, const Bytes& bank = TestBank())
{
    sdat::ArchiveModel model;
    sdat::Symbols symbols;
    symbols.names[0] = {name};
    symbols.names[2] = {"BANK_TEST"};
    symbols.names[3] = {"WAVE_TEST"};
    symbols.names[4] = {"PLAYER_BGM"};
    model.symbols = symbols;
    model.sequences = {sdat::SequenceInfo{0, 0, volume, 64, 64, 0, 0, ""}};
    model.banks = {sdat::BankInfo{1, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{2, ""}};
    model.players = {sdat::PlayerInfo{1, 0, channels, 0, ""}};
    model.files = {files::Sequence(bytecode), bank, TestWaveArchive()};

    return sdat::BuildArchive(model);
}

struct TestArchive
{
    explicit TestArchive(const Bytes& bytecode, uint16_t channels = 0, uint8_t volume = 127)
        : bytes(TestArchiveBytes(bytecode, "BGM_TEST", channels, volume)), archive(sdat::Archive::Parse(bytes))
    {
    }

    Bytes bytes;
    sdat::Archive archive;
};

// Note 60 at velocity 127 for 48 ticks, a wait of 48 ticks, and the end.
const Bytes kOneNote = {0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF};

// Note 60 for 24 ticks and a wait of 24 ticks, in a loop: a jump to the start.
const Bytes kLoop = {0x3C, 0x7F, 0x18, 0x80, 0x18, 0x94, 0x00, 0x00, 0x00};

// A program change to the program in variable 0 (the prefix 0xA1 takes the argument from a variable), then note 60 for
// 48 ticks, a wait of 48 ticks and the end.
const Bytes kProgramFromVariable = {0xA1, 0x81, 0x00, 0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF};

// The driver's command that a game's code sends for NNS_SndPlayerSetTrackVolume: the tracks of `tracks` of player 0
// take the fader `level` (SND_CalcDecibel's level of the volume, in 0.1 dB). Pokémon Black's code at 0x02005F24 sends
// the command in that form: {0x02000000, tracks, 0x0A, 0xFFFF8000} for volume 0, and 0 in place of 0xFFFF8000 for
// 127.
ntdlib::ntr::snd::Command TrackVolume(uint32_t tracks, int16_t level)
{
    return {.id = ntdlib::ntr::snd::CommandId::kTrackParameter,
            .args = {0x02000000, tracks, 0x0A, static_cast<uint32_t>(static_cast<int32_t>(level))}};
}

// A track's events of `kind`.
std::vector<seqlib::midi::TrackEvent> EventsOf(const Performance& p, int track, EventKind kind)
{
    std::vector<seqlib::midi::TrackEvent> events;
    for (const seqlib::midi::TrackEvent& e : p.tracks[static_cast<std::size_t>(track)])
    {
        if (e.kind == kind)
        {
            events.push_back(e);
        }
    }

    return events;
}

// The pan law. The DS's mixer plays the left side at (128 - p) / 128 and the right at p / 128, and the right at 128/128
// for a pan of 127. The sin/cos law at PanPosition, times PanGain, gives those levels.
void TestPanLaw()
{
    bool matches = true;
    for (int pan = 0; pan < 128; pan++)
    {
        const double right = pan == 127 ? 1.0 : pan / 128.0;
        const double angle = (PanPosition(pan) + 1.0) * std::numbers::pi / 4.0;
        matches = matches && std::abs(PanGain(pan) * std::cos(angle) - (1.0 - right)) < 1e-12 &&
                  std::abs(PanGain(pan) * std::sin(angle) - right) < 1e-12;
    }

    Check(matches, "the sin/cos law at PanPosition, times PanGain, gives the DS's levels of both sides");
    Check(PanPosition(0) == -1.0 && PanPosition(127) == 1.0 && std::abs(PanPosition(64)) < 1e-12,
          "the pans of 0, 64 and 127 are full left, the centre and full right");
    Check(PanGain(0) == 1.0 && PanGain(127) == 1.0 && std::abs(PanGain(64) - std::sqrt(0.5)) < 1e-12,
          "the centre plays at 0.707 of the sides");
}

// The envelopes. A decay or release of v falls 128 units of 0.1 dB a frame at the driver's rate: 0xFFFF for 127, v * 2
// + 1 below 50, and 0x1E00 / (126 - v) from 50 to 125. SoundFont's times give a fall of 100 dB, and SFZ's a fall of
// e^-8. A sustain of v plays at kDecibelSquare[v]: -11.9 dB for 64 and -1.0 dB for 120. An attack of a below 109
// multiplies the attenuation by (255 - a) / 256 each frame, from -92544. It reaches full level after 1653 frames for 0,
// 36 for 64 and 22 for 100. 109 and above take their factors from a table: 19 frames for 109 and 3 for 126.
void TestEnvelopes()
{
    const ntdlib::ntr::snd::Build build;
    const Sf2Envelope fastest = ConvertEnvelope({}, build);
    const Sf2Envelope envelope = ConvertEnvelope({.decay = 30, .sustain = 64, .release = 100}, build);
    const Sf2Envelope silent = ConvertEnvelope({.sustain = 0}, build);
    const Sf2Envelope slow = ConvertEnvelope({.decay = 0, .sustain = 120}, build);
    const SfzEnvelope sfz = ConvertEnvelopeForSfz({.decay = 30, .sustain = 64, .release = 100}, build);

    // 127's decay and release: 128000 / 0xFFFF frames, -7942.6 timecents. Its attack reaches full level in 1 frame.
    Check(fastest.attack == -12000 && fastest.hold == -9102 && fastest.decay == -7943 && fastest.sustain == 0 &&
              fastest.release == -7943,
          "the fastest envelope: no ramp, a frame of hold, and the fastest fall");
    // A decay of 30 falls 61 units a frame: 10931.5 ms for 100 dB. A release of 100 falls 295: 2260.4 ms.
    Check(envelope.decay == 4140 && envelope.sustain == 119 && envelope.release == 1412,
          "the decay's and release's times for 100 dB, and the sustain from the driver's table");
    Check(silent.sustain == 1440, "a sustain of 0 is silent");
    // A decay of 0 falls one unit a frame: 666,819 ms for 100 dB, slower than SoundFont's slowest. The hold gets half
    // the difference of the times that the two decays take to fall 1.0 dB.
    Check(slow.decay == 8000 && slow.hold == 1802, "a decay slower than SoundFont's holds longer");
    // SFZ's e^-8 is 69.49 dB: 88943.5 units.
    Check(std::abs(sfz.decay - 7.595961) < 1e-6 && std::abs(sfz.release - 1.570690) < 1e-6 &&
              std::abs(sfz.sustain - 25.409727) < 1e-6,
          "SFZ's decay and release for e^-8, and its sustain in percent");

    const int frames[] = {1653, 36, 22, 19, 3, 1};
    const uint8_t attacks[] = {0, 64, 100, 109, 126, 127};
    bool full = true;
    bool ramps = true;
    for (std::size_t i = 0; i < std::size(attacks); i++)
    {
        const SfzEnvelope e = ConvertEnvelopeForSfz({.attack = attacks[i]}, build);
        full = full && std::abs(e.attack + e.hold - frames[i] * kFrameMs / 1000.0) < 1e-9;
        ramps = ramps && e.attack >= 0.0 && e.hold >= 0.0 && (attacks[i] == 127 || e.attack > 0.0);
    }
    Check(full, "an attack's ramp and hold take the frames that the driver's attack takes to reach full level");
    Check(ramps, "every attack but 127's has a ramp");
}

// The zones of the bank's programs.
void TestPresetZones()
{
    const TestArchive test(kOneNote);
    BankSet banks(test.archive, 0);
    const sdat::Bank* bank = banks.Bank();
    Check(bank != nullptr && bank->instruments.size() == 8, "the bank reads");
    if (bank == nullptr || bank->instruments.size() != 8)
    {
        return;
    }

    const std::vector<PresetZone> pcm = PresetZones(*bank, {.program = 0});
    const std::vector<PresetZone> drums = PresetZones(*bank, {.program = 3});
    const std::vector<PresetZone> split = PresetZones(*bank, {.program = 4});
    const std::vector<PresetZone> region_envelope = PresetZones(*bank, {.program = 7});
    const std::vector<PresetZone> track_envelope = PresetZones(*bank, {.program = 7, .attack = 127, .release = 50});

    Check(pcm.size() == 1 && pcm[0].low_key == 0 && pcm[0].high_key == 127 && pcm[0].region.wave == 0,
          "a PCM instrument plays every key");
    Check(drums.size() == 3 && drums[0].low_key == 36 && drums[0].high_key == 36 && drums[0].region.root_key == 36 &&
              drums[1].low_key == 37 && drums[1].region.type == sdat::InstrumentType::kPsg && drums[2].low_key == 38 &&
              drums[2].region.type == sdat::InstrumentType::kNoise,
          "a drum set gives each of its keys a zone");
    Check(split.size() == 2 && split[0].low_key == 0 && split[0].high_key == 48 && split[0].region.wave == 0 &&
              split[1].low_key == 49 && split[1].high_key == 127 && split[1].region.type == sdat::InstrumentType::kPsg,
          "a key split plays the first region whose last key is at or above the note's");
    Check(PresetZones(*bank, {.program = 5}).empty() && PresetZones(*bank, {.program = 6}).empty() &&
              PresetZones(*bank, {.program = 8}).empty(),
          "a wave at an address, no instrument, and a program past the bank have no zones");
    Check(region_envelope.size() == 1 && region_envelope[0].envelope.attack == 100 &&
              region_envelope[0].envelope.decay == 30 && region_envelope[0].envelope.sustain == 64 &&
              region_envelope[0].envelope.release == 0 && region_envelope[0].region.pan == 0,
          "the region's envelope, with a release of 0xFF as 0");
    Check(track_envelope.size() == 1 && track_envelope[0].envelope.attack == 127 &&
              track_envelope[0].envelope.decay == 30 && track_envelope[0].envelope.release == 50,
          "the track's values take the place of the region's");
}

// The waves. A PCM wave plays at the rate of its timer: 16,756,991 Hz over the timer. The PSG's duty d holds its last
// d + 1 steps of 8 high, and duty 7 stays low. The noise's shift register starts at 0x7FFF. Each step shifts it right
// once, and a bit shifted out turns the output low and the register's bits 13 and 14 over: 14 low steps, then 10 high.
void TestWaves()
{
    const TestArchive test(kOneNote);
    BankSet banks(test.archive, 0);
    const auto region = [](sdat::InstrumentType type, uint16_t wave, uint16_t wave_archive = 0)
    {
        sdat::Region r;
        r.type = type;
        r.wave = wave;
        r.wave_archive = wave_archive;
        return r;
    };
    using enum sdat::InstrumentType;
    const Pcm* square = banks.Wave(region(kPcm, 0));
    const Pcm* ramp = banks.Wave(region(kPcm, 1));
    const Pcm* half = banks.Wave(region(kPsg, 3));
    const Pcm* eighth = banks.Wave(region(kPsg, 0));
    const Pcm* low = banks.Wave(region(kPsg, 7));
    const Pcm* noise = banks.Wave(region(kNoise, 0));
    const Pcm* no_slot = banks.Wave(region(kPcm, 0, 1));
    const Pcm* no_wave = banks.Wave(region(kPcm, 5));

    const auto runs = [](const Pcm* pcm, const std::vector<std::pair<int, int16_t>>& expected)
    {
        if (pcm == nullptr || pcm->channels.size() != 1)
        {
            return false;
        }
        std::size_t at = 0;
        for (const auto& [count, value] : expected)
        {
            for (int i = 0; i < count; i++, at++)
            {
                if (at >= pcm->channels[0].size() || pcm->channels[0][at] != value)
                {
                    return false;
                }
            }
        }
        return true;
    };
    const auto looped = [](const Pcm* pcm, std::size_t length)
    {
        return pcm != nullptr && pcm->channels[0].size() == length && pcm->loop && pcm->loop_start == 0 &&
               pcm->loop_end == length && pcm->sample_rate == kPsgSampleRate;
    };

    Check(square != nullptr && square->sample_rate == 32793 && square->loop && square->loop_start == 0 &&
              square->loop_end == 16 && runs(square, {{2, 16384}, {2, -16384}, {2, 16384}}),
          "a PCM8 wave plays at its timer's rate, and loops");
    Check(ramp != nullptr && ramp->sample_rate == 16396 && !ramp->loop && runs(ramp, {{1, 0}, {1, 0x1000}}) &&
              ramp->channels[0].size() == 8,
          "a PCM16 wave that plays once");
    Check(looped(half, 128) && runs(half, {{64, -0x7FFF}, {64, 0x7FFF}}), "duty 3 holds half its steps high");
    Check(looped(eighth, 128) && runs(eighth, {{112, -0x7FFF}, {16, 0x7FFF}}), "duty 0 holds one step high");
    Check(looped(low, 128) && runs(low, {{128, 0}}), "duty 7 stays low");
    Check(looped(noise, std::size_t{0x7FFF} * 16) && runs(noise, {{14 * 16, -0x7FFF}, {10 * 16, 0x7FFF}}),
          "the noise's steps");
    Check(banks.WaveName(region(kPcm, 1)) == "w0_1" && banks.WaveName(region(kPsg, 3)) == "psg3" &&
              banks.WaveName(region(kNoise, 0)) == "noise" && banks.WaveName(region(kPcm, 0, 1)).empty(),
          "the waves' names");
    Check(no_slot == nullptr && no_wave == nullptr &&
              banks.Warnings() == std::vector<std::string>{"bank 0 has no wave archive in its slot 1",
                                                           "w0_5 can't be read: wave archive 0 has no wave 5"},
          "a missing wave archive and a missing wave give warnings");
}

// The performer. The driver's tempo of 120 adds 120 a frame, and a tick takes 240: a tick every two frames.
void TestPerform()
{
    const TestArchive one_note(kOneNote);
    const Performance p = Perform(one_note.archive, {0});
    const std::vector<seqlib::midi::TrackEvent> ons = EventsOf(p, 0, EventKind::kNoteOn);
    const std::vector<seqlib::midi::TrackEvent> offs = EventsOf(p, 0, EventKind::kNoteOff);

    Check(p.bank == 0 && p.presets.size() == 1 && p.presets[0] == PresetKey{}, "one preset, of program 0");
    Check(ons.size() == 1 && ons[0].tick == 0 && ons[0].key == 60 && ons[0].value == 127,
          "the note starts at tick 0, at its velocity");
    Check(offs.size() == 1 && offs[0].tick == 48, "and ends at tick 48");
    Check(!p.tempo.empty() && std::abs(p.tempo[0].bpm - 60000.0 / (96.0 * kFrameMs)) < 1e-9 && p.timebase == 48,
          "the tempo: a tick every two frames");
    Check(!p.loop && p.loops == 0 && !p.truncated, "the sequence ends");

    // A track that ends more than 10 s after its last sound has died away: the note, a wait of 2000 ticks (20.9 s) and
    // the end. The performance ends where the sound dies away. After a wait of 900 ticks (9.4 s), it ends with the
    // track.
    const TestArchive late_end({0x3C, 0x7F, 0x30, 0x80, 0x8F, 0x50, 0xFF});
    const TestArchive end_in_time({0x3C, 0x7F, 0x30, 0x80, 0x87, 0x04, 0xFF});
    const Performance late = Perform(late_end.archive, {0});
    const Performance in_time = Perform(end_in_time.archive, {0});
    Check(EventsOf(late, 0, EventKind::kNoteOff).size() == 1 && late.end_tick >= 48 && late.end_tick < 96 &&
              late.seconds < 1.0 && !late.truncated,
          "a track's end long after the last sound: the performance ends at the sound");
    Check(in_time.end_tick == 948 && !in_time.truncated, "a track's end within 10 s of it: the performance ends there");

    // The archive's volume of the sequence. At 64, the SDK gives the player a fader of -60: -6.0 dB, the level of
    // 64/127. CC11 holds the square root of the gain. At the centre, the note's CC11 falls from 127 times
    // sqrt(PanGain(64)), 107, to that times 10^(-60/400), 76.
    const TestArchive quieter(kOneNote, 0, 64);
    const std::vector<seqlib::midi::TrackEvent> full = EventsOf(p, 0, EventKind::kExpression);
    const std::vector<seqlib::midi::TrackEvent> quiet =
        EventsOf(Perform(quieter.archive, {0}), 0, EventKind::kExpression);
    Check(!full.empty() && full[0].tick == 0 && full[0].value == 107 && !quiet.empty() && quiet[0].tick == 0 &&
              quiet[0].value == 76,
          "the sequence's volume in CC11: 107 at 127, 76 at 64");

    // A game's commands once the sequence has started run in its first frame. A track volume of 0 gives track 0 a
    // fader of -32768, and its CC11 falls to 0. A later command for the same track takes the place of the first. A
    // tempo's ratio of 128 (the player's parameter at 0x1A, 2 bytes) halves the tempo: a tick every four frames.
    const std::vector<seqlib::midi::TrackEvent> silent = EventsOf(
        Perform(one_note.archive, {0}, {.commands = {TrackVolume(0x0001, -32768)}}), 0, EventKind::kExpression);
    const std::vector<seqlib::midi::TrackEvent> restored =
        EventsOf(Perform(one_note.archive, {0}, {.commands = {TrackVolume(0x0001, -32768), TrackVolume(0x0001, 0)}}), 0,
                 EventKind::kExpression);
    Check(!silent.empty() && silent[0].tick == 0 && silent[0].value == 0 && !restored.empty() &&
              restored[0].tick == 0 && restored[0].value == 107,
          "a game's track volume after the start: CC11 0 at volume 0, and 107 once a later command gives 127");
    const ntdlib::ntr::snd::Command half_tempo = {.id = ntdlib::ntr::snd::CommandId::kPlayerParameter,
                                                  .args = {0, 0x1A, 128, 2}};
    const Performance slow = Perform(one_note.archive, {0}, {.commands = {half_tempo}});
    Check(!slow.tempo.empty() && std::abs(slow.tempo[0].bpm - 60000.0 / (192.0 * kFrameMs)) < 1e-9,
          "a game's tempo ratio of 128 after the start: a tick every four frames");

    // A game's channels for the tracks: a track that may take no channel plays no note. The mask of another track
    // leaves track 0 alone.
    const Performance no_channels = Perform(one_note.archive, {0}, {.track_channels = {{0x0001, 0}}});
    const Performance other_track = Perform(one_note.archive, {0}, {.track_channels = {{0x0002, 0}}});
    Check(EventsOf(no_channels, 0, EventKind::kNoteOn).empty() &&
              EventsOf(other_track, 0, EventKind::kNoteOn).size() == 1,
          "a track takes only the channels that its mask gives");

    // At a tempo of 100, a tick takes 2.4 frames. The player's counter starts at 240. Each frame runs a tick while the
    // counter holds 240, then adds 100. Ticks 1 to 5 run in frames 3, 5, 8, 10 and 12, from 0.6, 0.2, 0.8, 0.4 and 0
    // frames after their times at MIDI's tempo.
    const TestArchive slower({0xE1, 0x64, 0x00, 0x3C, 0x7F, 0x30, 0xFF});
    const Performance hundred = Perform(slower.archive, {0});
    const double starts[] = {0.0, 1.25, 2.0 + 0.2 / 2.4, 3.0 + 0.8 / 2.4, 4.0 + 0.4 / 2.4, 5.0};
    bool on_frames = hundred.frame_ticks.size() > std::size(starts);
    for (std::size_t k = 0; on_frames && k < std::size(starts); k++)
    {
        on_frames = std::abs(hundred.frame_ticks[k] - starts[k]) < 1e-9;
    }
    Check(!hundred.tempo.empty() && std::abs(hundred.tempo[0].bpm - 60000.0 / (48 * 2.4 * kFrameMs)) < 1e-9,
          "a tempo of 100: a tick every 2.4 frames");
    Check(on_frames, "each tick's events go at the start of the frame that runs it");

    // A pass of the loop takes 48 ticks: 96 frames, 500.1 ms. The performance plays passes for 10 s at least.
    const TestArchive loop(kLoop);
    const Performance looped = Perform(loop.archive, {0});
    const Performance twice = Perform(loop.archive, {0}, {.loops = 2});
    Check(looped.loop && looped.loop->start == 0 && looped.loop->end == 48, "the loop's markers on its first pass");
    Check(looped.loops == 20 && looped.end_tick == 960 && EventsOf(looped, 0, EventKind::kNoteOn).size() == 20,
          "20 passes reach 10 s");
    Check(twice.loop && twice.loop->start == 0 && twice.loop->end == 48 && twice.loops == 20,
          "the 10 s hold more than two passes");

    // Ties (0xC8 1), then an endless loop (0xD4 0 to 0xFC) of note 60 for 24 ticks. Each pass ties the note again and
    // changes nothing. The sound holds until the game stops it. The conversion ends 10 s after the sound settles.
    const TestArchive tie({0xC8, 0x01, 0xD4, 0x00, 0x3C, 0x7F, 0x18, 0xFC, 0xFF});
    const Performance tied = Perform(tie.archive, {0});
    Check(tied.holds && !tied.loop && !tied.truncated && EventsOf(tied, 0, EventKind::kNoteOn).size() == 1,
          "a tie struck again in each pass holds, without loop markers");
    Check(tied.seconds >= 10.5 && tied.seconds < 10.5 + 2.0 * kFrameMs / 1000.0,
          "the hold ends 10 s after the second pass, where the sound settles");

    // Program 7, an attack of 100 and a release of 50, then the note.
    const TestArchive program({0x81, 0x07, 0xD0, 100, 0xD3, 50, 0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF});
    const Performance seven = Perform(program.archive, {0});
    Check(seven.presets.size() == 1 && seven.presets[0] == PresetKey{.program = 7, .attack = 100, .release = 50},
          "the preset takes the program and the track's envelope");

    // The note plays only when variable 0 is 1: a comparison (0xB8) and the if prefix (0xA2).
    const TestArchive on_variable({0xB8, 0x00, 0x01, 0x00, 0xA2, 0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF});
    const Performance unset = Perform(on_variable.archive, {0});
    const Performance set = Perform(on_variable.archive, {0}, {.variables = {{0, 1}}});
    Check(EventsOf(unset, 0, EventKind::kNoteOn).empty() && EventsOf(set, 0, EventKind::kNoteOn).size() == 1,
          "a variable set before the sequence starts");

    // A wait of 1 to 96 ticks drawn at random (the random prefix, 0xA0), then a note. The seed sets the driver's
    // random numbers.
    const TestArchive drawn({0xA0, 0x80, 0x01, 0x00, 0x60, 0x00, 0x3C, 0x7F, 0x30, 0xFF});
    const auto start = [&drawn](uint32_t seed)
    {
        const std::vector<seqlib::midi::TrackEvent> on =
            EventsOf(Perform(drawn.archive, {0}, {.seed = seed}), 0, EventKind::kNoteOn);
        return on.size() == 1 ? static_cast<int>(on[0].tick) : -1;
    };
    const int first = start(1);
    const int second = start(2);
    Check(first >= 1 && first <= 96 && second >= 1 && second <= 96 && first != second && start(1) == first,
          "the seed draws the wait: " + std::to_string(first) + " and " + std::to_string(second));
}

// A note whose channel another note takes. The player has channel 0 alone. Track 0 plays note 60 for 96 ticks. At
// tick 24, track 1 plays note 64 at a higher priority, and takes the channel. Track 0's note ends at once. Nothing else
// of track 0 sounds on its part. CC120 then stops the note in the synthesizer.
// A game's changes of a variable while the sequence plays. The track plays a note in each pass where variable 0 holds
// 5. A tick runs every second frame. A change at frame 3,000, about 15.6 s in, comes at tick 1,500. That lies past the
// 10 s after which the performer stops at a steady pass without a change.
void TestChanges()
{
    const TestArchive sometimes({
        0xB8,
        0x00,
        0x05,
        0x00, // 0: whether variable 0 is 5
        0xA2,
        0x3C,
        0x7F,
        0x18, // 4: if so, a note of 24 ticks
        0x80,
        0x18, // 8: a wait of 24 ticks
        0x94,
        0x00,
        0x00,
        0x00, // 10: back to 0
    });

    const Performance without = Perform(sometimes.archive, {0});
    Check(EventsOf(without, 0, EventKind::kNoteOn).empty(), "without a change, the note never plays");

    const Performance changed = Perform(sometimes.archive, {0}, {.changes = {{3000, 0, 5}}});
    const std::vector<seqlib::midi::TrackEvent> ons = EventsOf(changed, 0, EventKind::kNoteOn);
    Check(!ons.empty() && ons[0].tick >= 1500 && ons[0].tick < 1524,
          "the change plays the note from the first pass after its tick");
    Check(changed.loop && changed.loop->start >= 1500,
          "the performance plays on past the change, and its loop starts after it");

    // The changes come in the order of their frames, whatever the list's order. The last one sets the variable back,
    // and the notes stop.
    const Performance back = Perform(sometimes.archive, {0}, {.changes = {{3000, 0, 1}, {2000, 0, 5}}});
    const std::vector<seqlib::midi::TrackEvent> between = EventsOf(back, 0, EventKind::kNoteOn);
    Check(!between.empty() && between.front().tick >= 1000 && between.front().tick < 1024 && between.back().tick < 1500,
          "two changes: the note plays only between them");

    // A global variable, by index 16 on.
    const TestArchive global({0xB8, 0x10, 0x05, 0x00, 0xA2, 0x3C, 0x7F, 0x18, 0x80, 0x18, 0x94, 0x00, 0x00, 0x00});
    const Performance from_global = Perform(global.archive, {0}, {.changes = {{100, 16, 5}}});
    Check(!EventsOf(from_global, 0, EventKind::kNoteOn).empty(), "a change of a global variable reaches the track");
}

void TestStolenChannel()
{
    // 0: alloc tracks 0 and 1; 3: open track 1 at 14; 8: note 60 for 96; 11: wait 96; 13: end.
    // 14: wait 24; 16: priority 127; 18: note 64 for 24; 21: wait 24; 23: end.
    const Bytes alone = {0xFE, 0x03, 0x00, 0x93, 0x01, 0x0E, 0x00, 0x00, 0x3C, 0x7F, 0x60, 0x80,
                         0x60, 0xFF, 0x80, 0x18, 0xC6, 0x7F, 0x40, 0x7F, 0x18, 0x80, 0x18, 0xFF};
    const TestArchive test(alone, 0x0001);
    const Performance p = Perform(test.archive, {0});
    const std::vector<seqlib::midi::TrackEvent> offs = EventsOf(p, 0, EventKind::kNoteOff);
    const std::vector<seqlib::midi::TrackEvent> stops = EventsOf(p, 0, EventKind::kSoundOff);
    const std::vector<seqlib::midi::TrackEvent> taken = EventsOf(p, 1, EventKind::kNoteOn);

    Check(taken.size() == 1 && taken[0].tick == 24, "track 1's note starts at tick 24");
    Check(offs.size() == 1 && offs[0].tick == 24, "track 0's note ends there");
    Check(stops.size() == 1 && stops[0].tick == 24 && p.approximations.empty(), "and CC120 stops its sound");

    // With two channels, track 0 turns its notes' waits off and plays a chord of notes 60 and 67. Track 1 takes one of
    // their channels. The other note still sounds on the part, and CC120 would stop it too. The note that ends rings
    // on in its release.
    // 0: alloc tracks 0 and 1; 3: open track 1 at 19; 8: no note waits; 10: note 60 for 96; 13: note 67 for 96;
    // 16: wait 96; 18: end. 19: wait 24; 21: priority 127; 23: note 64 for 24; 26: wait 24; 28: end.
    const Bytes chord = {0xFE, 0x03, 0x00, 0x93, 0x01, 0x13, 0x00, 0x00, 0xC7, 0x00, 0x3C, 0x7F, 0x60, 0x43, 0x7F,
                         0x60, 0x80, 0x60, 0xFF, 0x80, 0x18, 0xC6, 0x7F, 0x40, 0x7F, 0x18, 0x80, 0x18, 0xFF};
    const TestArchive two(chord, 0x0003);
    const Performance q = Perform(two.archive, {0});
    const std::vector<seqlib::midi::TrackEvent> chord_offs = EventsOf(q, 0, EventKind::kNoteOff);

    Check(EventsOf(q, 0, EventKind::kSoundOff).empty() && chord_offs.size() == 2 && chord_offs[0].tick == 24 &&
              chord_offs[1].tick == 96,
          "a note whose channel another note takes ends; the other note plays on");
    Check(q.approximations.size() == 1 &&
              q.approximations.begin()->first == "note cut short by another note's channel (rings on in its release)",
          "the release that the synthesizer plays counts as an approximation");
}

// A conversion's files pass seqlib's checks, and each tick's frame starts no earlier than the tick, within a frame of
// it: half a tick at a tempo of 120. The driver runs a tick at the start of the frame after the tick's time.
// sequence_export's other consoles run it before (ntdlib::testing::CheckConversion).
void CheckFiles(const Conversion& c, const std::string& name)
{
    const std::vector<std::string> midi = seqlib::midi::Problems(c.midi);
    const std::vector<std::string> sf2 = seqlib::sf2::Check(c.sf2);
    const std::string sfz =
        midi.empty() ? ntdlib::testing::SfzProblem(c.sfz, seqlib::midi::TrackNames(seqlib::midi::Read(c.midi))) : "";
    Check(midi.empty() && sf2.empty() && sfz.empty(),
          name + ": seqlib's checks: " + (midi.empty() ? "" : midi[0]) + (sf2.empty() ? "" : sf2[0]) + sfz);

    const std::vector<double>& frames = c.performance.frame_ticks;
    bool ordered = !frames.empty();
    for (std::size_t k = 0; k < frames.size(); k++)
    {
        ordered = ordered && frames[k] > static_cast<double>(k) - 1e-9 &&
                  frames[k] < static_cast<double>(k) + 0.5 + 1e-9 && (k == 0 || frames[k] >= frames[k - 1]);
    }
    Check(ordered, name + ": each tick's frame");
}

// The conversion of a sequence that plays every kind of instrument, with each kind of LFO and a sweep. Program 7's
// region plays full left, 3 dB above the centre. The SoundFont's zones are then 3 dB quieter.
void TestConvert()
{
    // clang-format off
    const Bytes every = {
        0x81, 0x00, 0x3C, 0x7F, 0x18, 0x80, 0x18,                               // wave 0
        0x81, 0x01, 0x45, 0x7F, 0x18, 0x80, 0x18,                               // the PSG
        0x81, 0x02, 0x3C, 0x7F, 0x18, 0x80, 0x18,                               // the noise
        0x81, 0x03, 0x24, 0x7F, 0x0C, 0x25, 0x7F, 0x0C, 0x26, 0x7F, 0x0C, 0x80, 0x18, // the drum set
        0x81, 0x04, 0x30, 0x7F, 0x0C, 0x80, 0x0C, 0x48, 0x7F, 0x0C, 0x80, 0x0C, // the key split
        0x81, 0x07, 0xD3, 0x7F, 0xC0, 0x20, 0xCA, 0x40, 0xCB, 0x10, 0xCD, 0x02, // release 127, pan 32, a pitch LFO
        0x3C, 0x7F, 0x30, 0x80, 0x30,
        0xCC, 0x01, 0xCA, 0x7F, 0x3C, 0x7F, 0x30, 0x80, 0x30,                   // a volume LFO
        0xCC, 0x02, 0x3C, 0x7F, 0x30, 0x80, 0x30,                               // a pan LFO
        0xCA, 0x00, 0xE3, 0x40, 0x00, 0x3C, 0x7F, 0x30, 0x80, 0x30,             // a sweep
        0xFF,
    };
    // clang-format on
    const TestArchive test(every);
    const Conversion c = ConvertSequence(test.archive, {0}, "BGM_TEST", {}, kProgram);
    CheckFiles(c, "every instrument");

    // CC11's level is (v / 127)^2. It takes the gain of the track's pan: 127 * sqrt(PanGain(64)) = 106.8 at the
    // centre, and 127 * sqrt(PanGain(32)) = 112.9 at 32. CC10 at 32 is 64 + 64 * PanPosition(32) = 26.2.
    std::vector<std::pair<uint32_t, int>> controls;
    for (const seqlib::midi::TrackEvent& e : c.performance.tracks[0])
    {
        if (e.kind == EventKind::kExpression || e.kind == EventKind::kPan)
        {
            controls.emplace_back(e.tick, e.kind == EventKind::kPan ? 1000 + e.value : e.value);
        }
    }
    Check(controls == std::vector<std::pair<uint32_t, int>>{{0, 107}, {0, 1064}, {252, 113}, {252, 1026}},
          "CC11 holds the pan's gain, and CC10 the pan");

    // The LFO at speed 16 steps through 512 / 16 frames a cycle: 5.998625 Hz. At CC1's 127 and range 2, the pitch
    // swings by 127 * 127 * 2 / 16384 semitones, and the volume by 60 times that in 0.1 dB. SoundFont's CC1 gives
    // 127/128 of a modulator's amount. Program 7's region plays full left, 3.01 dB above the centre.
    const std::string& text = c.sfz.files.empty() ? std::string() : c.sfz.files[0].text;
    Check(text.find("lfo1_freq=5.998625 lfo1_pitch_oncc1=196.887207") != std::string::npos &&
              text.find("lfo1_volume_oncc1=11.813232") != std::string::npos &&
              text.find("lfo1_pan_oncc1=196.887207") != std::string::npos,
          "SFZ's LFOs");
    Check(text.find("<region> sample=w0_1.wav lokey=0 hikey=127 pitch_keycenter=60 sample_quality=0 volume=3.01 "
                    "pan=-100 ") != std::string::npos,
          "SFZ's region full left");
    const seqlib::sf2::Records records = seqlib::sf2::Read(c.sf2);
    bool pitch = false;
    bool volume = false;
    for (std::size_t i = 0; i < records.Count("imod", 10); i++)
    {
        const uint8_t* r = records.Record("imod", 10, i);
        const auto field = [r](int at)
        {
            return xsflib::LoadLe16(r + at);
        };
        pitch = pitch || (field(0) == 0x0081 && field(2) == 6 && field(4) == 198);
        volume = volume || (field(0) == 0x0081 && field(2) == 13 && field(4) == 119);
    }
    Check(pitch && volume, "SoundFont's modulators from CC1: 198.4 cents and 119.1 cB");

    // Program 7's zones pan full left (-500). The other zones stay at the centre.
    std::size_t left = 0;
    std::size_t panned = 0;
    for (std::size_t z = 0; z + 1 < records.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> generators = records.ZoneGens(z);
        if (const auto pan = generators.find(seqlib::sf2::kPan); pan != generators.end() && pan->second != 0)
        {
            panned++;
            left += static_cast<int16_t>(pan->second) == -500 ? 1 : 0;
        }
    }
    Check(panned == 3 && left == 3, "SoundFont's zones of program 7's three presets pan full left");

    // Wave 0, wave 1, the PSG at duties 3, 1 and 0, and the noise.
    Check(c.samples == 6 && c.sfz.samples.size() == 6, "a sample for each wave, and a WAV file for each");
    Check(c.warnings.empty(), "no warnings");
    Check(c.performance.approximations ==
              std::map<std::string, uint32_t>{
                  {"instrument volume above full level (SoundFont attenuated by 3.0 dB; SFZ keeps the level)", 12},
                  {"pan LFO (omitted from SoundFont)", 1}},
          "the cut of the SoundFont, and the pan LFO in SFZ alone");

    const TestArchive loop(kLoop);
    CheckFiles(ConvertSequence(loop.archive, {0}, "BGM_LOOP", {.loops = 2}, kProgram), "a loop");
    const TestArchive one_note(kOneNote);
    CheckFiles(ConvertSequence(one_note.archive, {0}, "BGM_TEST", {}, kProgram), "one note");
}

// The phdr records of SoundFont `sf2`: each preset's bank and program, in order, without the terminal record.
std::vector<std::pair<uint16_t, uint16_t>> PresetsOf(const std::vector<uint8_t>& sf2)
{
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    std::vector<std::pair<uint16_t, uint16_t>> presets;
    for (std::size_t i = 0; i + 1 < records.Count("phdr", 38); i++)
    {
        const uint8_t* r = records.Record("phdr", 38, i);
        presets.emplace_back(xsflib::LoadLe16(r + 22), xsflib::LoadLe16(r + 20));
    }

    return presets;
}

// The conversion of the test bank's instruments by themselves. Programs 5 and 6 play nothing, and neither does a
// program past the bank. Program 7's region plays full left, 3 dB above the centre, and the SoundFont's zones are then
// 3 dB quieter. A program past 127 takes a preset in bank 1. Programs given out of order, or twice, convert in
// ascending order, once each.
void TestConvertBank()
{
    const TestArchive test(kOneNote);
    const std::vector<uint32_t> every = {0, 1, 2, 3, 4, 5, 6, 7, 200};
    const ntdlib::sequence_export::BankConversion c =
        ConvertBank(test.archive, 0, every, ntdlib::ntr::snd::Build{}, kProgram);
    Check(c.sound_fonts.size() == 1 && c.sound_fonts[0].first == 0 && c.sound_fonts[0].last == 7,
          "a bank: one SoundFont of programs 0 to 7");
    const std::vector<uint8_t>& sf2 = c.sound_fonts.empty() ? std::vector<uint8_t>() : c.sound_fonts[0].sf2;
    Check(seqlib::sf2::Check(sf2).empty(), "a bank: seqlib's check of the SoundFont");
    Check(PresetsOf(sf2) == std::vector<std::pair<uint16_t, uint16_t>>{{0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 7}},
          "a bank: a preset at each program that plays");

    // The zones of programs 0 to 4 play at the centre, 3 dB quieter. The drum set has three, and the key split two.
    // Program 7's zone plays full left, at full level.
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    std::map<uint16_t, std::size_t> attenuations;
    for (std::size_t z = 0; z + 1 < records.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> generators = records.ZoneGens(z);
        if (generators.contains(seqlib::sf2::kSampleId))
        {
            const auto found = generators.find(seqlib::sf2::kInitialAttenuation);
            attenuations[found != generators.end() ? found->second : 0]++;
        }
    }
    Check(attenuations.size() == 2 && attenuations.begin()->first == 0 && attenuations.begin()->second == 1 &&
              attenuations.rbegin()->second == 8,
          "a bank: the SoundFont's zones take the cut, but for the zone full left");

    // Wave 0, wave 1, the PSG at duties 3, 1 and 0, and the noise.
    Check(!c.sound_fonts.empty() && c.sound_fonts[0].samples == 6 && c.sfz.samples.size() == 6,
          "a bank: a sample for each wave, and a WAV file for each");
    std::vector<std::string> names = {"conductor"};
    for (const seqlib::sfz::File& file : c.sfz.files)
    {
        names.push_back(file.name.substr(0, file.name.size() - 4));
    }
    Check(names == std::vector<std::string>{"conductor", "BANK_TEST_0", "BANK_TEST_1", "BANK_TEST_2", "BANK_TEST_3",
                                            "BANK_TEST_4", "BANK_TEST_7"},
          "a bank: an SFZ file of each program that plays");
    Check(ntdlib::testing::SfzProblem(c.sfz, names).empty(), "a bank: the SFZ files' samples and loops");
    const std::string& seventh = c.sfz.files.size() == 6 ? c.sfz.files[5].text : std::string();
    Check(seventh.find("<region> sample=w0_1.wav lokey=0 hikey=127 pitch_keycenter=60 sample_quality=0 volume=3.01 "
                       "pan=-100 ") != std::string::npos &&
              seventh.find("bend_up=200 bend_down=-200") != std::string::npos &&
              seventh.find("// Program 7 of BANK_TEST, converted by xyz2midi.") == 0,
          "a bank: program 7's file plays its region full left, with a track's starting bend range");
    const std::string& drums = c.sfz.files.size() == 6 ? c.sfz.files[3].text : std::string();
    std::size_t regions = 0;
    for (std::size_t at = drums.find("<region> "); at != std::string::npos; at = drums.find("<region> ", at + 1))
    {
        regions++;
    }
    Check(regions == 3 && drums.find("lokey=37 hikey=37") != std::string::npos, "a bank: the drum set's three keys");
    Check(
        c.warnings == std::vector<std::string>{"program 5 of bank 0 plays nothing", "program 6 of bank 0 plays nothing",
                                               "program 200 of bank 0 plays nothing",
                                               "instrument volume above full level (SoundFont attenuated by 3.0 dB; "
                                               "SFZ keeps the level)"},
        "a bank: the programs that play nothing, and the cut of the SoundFont");

    // Program 7 by itself, then program 130 of a bank of 131 programs.
    const ntdlib::sequence_export::BankConversion seventh_only =
        ConvertBank(test.archive, 0, std::vector<uint32_t>{7}, ntdlib::ntr::snd::Build{}, kProgram);
    Check(seventh_only.sound_fonts.size() == 1 &&
              PresetsOf(seventh_only.sound_fonts[0].sf2) == std::vector<std::pair<uint16_t, uint16_t>>{{0, 7}} &&
              seventh_only.sfz.files.size() == 1 && seventh_only.sfz.files[0].name == "BANK_TEST_7.sfz" &&
              seventh_only.sfz.samples.size() == 1,
          "a bank: one program by itself");
    std::vector<files::Instrument> many(131);
    many.back() = {.type = 1, .region = {.wave = 1}};
    const Bytes large_bytes = TestArchiveBytes(kOneNote, "BGM_TEST", 0, 127, files::Bank(many));
    const sdat::Archive large = sdat::Archive::Parse(large_bytes);
    const ntdlib::sequence_export::BankConversion past =
        ConvertBank(large, 0, std::vector<uint32_t>{9, 130}, ntdlib::ntr::snd::Build{}, kProgram);
    Check(past.sound_fonts.size() == 1 &&
              PresetsOf(past.sound_fonts[0].sf2) == std::vector<std::pair<uint16_t, uint16_t>>{{1, 2}} &&
              past.sfz.files.size() == 1 && past.sfz.files[0].name == "BANK_TEST_130.sfz",
          "a bank: program 130 takes preset 2 of bank 1");
    const ntdlib::sequence_export::BankConversion unsorted =
        ConvertBank(large, 0, std::vector<uint32_t>{130, 9, 130}, ntdlib::ntr::snd::Build{}, kProgram);
    Check(unsorted.sound_fonts.size() == 1 && unsorted.sound_fonts[0].first == 130 &&
              PresetsOf(unsorted.sound_fonts[0].sf2) == std::vector<std::pair<uint16_t, uint16_t>>{{1, 2}} &&
              unsorted.sfz.files.size() == 1 && unsorted.sfz.files[0].name == "BANK_TEST_130.sfz",
          "a bank: a program given twice converts once");
    const ntdlib::sequence_export::BankConversion repeated =
        ConvertBank(test.archive, 0, std::vector<uint32_t>{7, 0, 7}, ntdlib::ntr::snd::Build{}, kProgram);
    std::vector<std::string> repeated_names;
    for (const seqlib::sfz::File& file : repeated.sfz.files)
    {
        repeated_names.push_back(file.name);
    }
    Check(repeated.sound_fonts.size() == 1 && repeated.sound_fonts[0].first == 0 && repeated.sound_fonts[0].last == 7 &&
              PresetsOf(repeated.sound_fonts[0].sf2) == std::vector<std::pair<uint16_t, uint16_t>>{{0, 0}, {0, 7}} &&
              repeated_names == std::vector<std::string>{"BANK_TEST_0.sfz", "BANK_TEST_7.sfz"},
          "a bank: programs out of order convert in ascending order");
    many[7] = many.back();
    const Bytes wide_bytes = TestArchiveBytes(kOneNote, "BGM_TEST", 0, 127, files::Bank(many));
    const sdat::Archive wide = sdat::Archive::Parse(wide_bytes);
    const ntdlib::sequence_export::BankConversion widths =
        ConvertBank(wide, 0, std::vector<uint32_t>{130, 7}, ntdlib::ntr::snd::Build{}, kProgram);
    std::vector<std::string> width_names;
    for (const seqlib::sfz::File& file : widths.sfz.files)
    {
        width_names.push_back(file.name);
    }
    Check(widths.sound_fonts.size() == 1 &&
              PresetsOf(widths.sound_fonts[0].sf2) == std::vector<std::pair<uint16_t, uint16_t>>{{0, 7}, {1, 2}} &&
              width_names == std::vector<std::string>{"BANK_TEST_007.sfz", "BANK_TEST_130.sfz"},
          "a bank: programs of one and three digits, given from the highest");

    bool threw = false;
    try
    {
        ConvertBank(test.archive, 1, every, ntdlib::ntr::snd::Build{}, kProgram);
    }
    catch (const xsflib::FormatError&)
    {
        threw = true;
    }
    Check(threw, "a bank that the archive lacks");
}

// The command line on a cartridge image with two sound archives in one file and a third in another, on a loose sound
// archive, on a file that is neither, and on a cartridge image with DSE's songs and sound effects.
void TestFrame()
{
    const ntdlib::testing::TempFolder folder("sequence_export_ntr_test");
    const Bytes one_note = TestArchiveBytes(kOneNote);
    const Bytes loop = TestArchiveBytes(kLoop, "BGM_LOOP");
    Bytes pack = {'J', 'U', 'N', 'K', 0, 0, 0, 0};
    pack.insert(pack.end(), loop.begin(), loop.end());
    files::Align(pack, 4);
    pack.insert(pack.end(), one_note.begin(), one_note.end());

    nds_files::Image spec;
    spec.files = {{"sound/sound_data.sdat", one_note}, {"data/pack.bin", pack}, {"data/readme.txt", {'h', 'i'}}};
    const fs::path image = folder.Write("game.nds", nds_files::BuildImage(spec).first);
    const fs::path loose = folder.Write("loose.sdat", one_note);
    const fs::path neither = folder.Write("neither.bin", {1, 2, 3, 4, 5, 6, 7, 8});

    ArchiveConverter converter;
    converter.program = kProgram;
    converter.version_line = "xyz2midi 1.0";
    const auto run = [&converter](const fs::path& input)
    {
        return RunArchiveConverter({kProgram, "--quiet", ntdlib::sequence_export::Utf8(input)}, converter);
    };

    // Each file's archives go to folders named after the file: a second archive in the file takes _1.
    const fs::path out = folder.Path() / "game_xyz2midi";
    Check(run(image) == 0, "the image converts");
    const auto passes = [](const fs::path& stem)
    {
        const std::optional<std::vector<uint8_t>> midi =
            ntdlib::sequence_export::ReadFile(fs::path(stem).replace_extension(".mid"));
        const std::optional<std::vector<uint8_t>> sf2 =
            ntdlib::sequence_export::ReadFile(fs::path(stem).replace_extension(".sf2"));
        return midi && sf2 && seqlib::midi::Problems(*midi).empty() && seqlib::sf2::Check(*sf2).empty();
    };
    Check(passes(out / "sound_data" / "BGM_TEST") && passes(out / "pack" / "BGM_LOOP") &&
              passes(out / "pack_1" / "BGM_TEST"),
          "each archive's files, which pass seqlib's checks");
    Check(fs::exists(out / "pack" / "sfz" / "BGM_LOOP" / "Track 0.sfz") && fs::exists(out / "pack" / "sfz" / "samples"),
          "and its SFZ files");

    Check(run(loose) == 0 && passes(folder.Path() / "loose_xyz2midi" / "BGM_TEST"), "a loose archive converts");

    // DSE's songs and sound effects: each folder of them goes to a folder of its name. A sound effect takes the name
    // of its bank of sound effects and its slot.
    nds_files::Image dse;
    dse.files = {
        {"SOUND/BGM/bgm0001.smd", dse_song_files::SongFile()},
        {"SOUND/BGM/bgm0001.swd", dse_song_files::BankFile()},
        {"SOUND/BGM/bgm.swd", dse_song_files::MainBankFile()},
        {"SOUND/SE/fx.sed",
         dse_song_files::EffectsFile(
             {std::nullopt,
              dse_song_files::Effect{.tracks = {dse_song_files::Join({{1, 0, 0, 0},
                                                                      {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F},
                                                                      dse_song_files::Note(0x60, 0, 0, 0x18, 1),
                                                                      {0x92, 0x18, 0x98}})}}},
             5)},
        {"SOUND/SE/fx.swd", dse_song_files::BankFile()}};
    const fs::path dse_image = folder.Write("dse.nds", nds_files::BuildImage(dse).first);
    const fs::path dse_out = folder.Path() / "dse_xyz2midi";
    Check(run(dse_image) == 0 && passes(dse_out / "BGM" / "bgm0001") && passes(dse_out / "SE" / "fx_001") &&
              fs::exists(dse_out / "SE" / "sfz" / "fx_001" / "Track 0.sfz"),
          "DSE's songs and sound effects convert");
    Check(run(neither) == 1 && !fs::exists(folder.Path() / "neither_xyz2midi"), "a file that is neither fails");

    // A game's profile: its settings give a sequence's program through variable 0, and an arrangement gives another
    // program. Without a value, the program is -1 and the sequence plays nothing.
    const fs::path voices = folder.Write("voices.sdat", TestArchiveBytes(kProgramFromVariable, "SE_VOICE"));
    ArchiveConverter profiled = converter;
    profiled.game_settings = [](const ArchiveInput&, const ntdlib::ntr::SequenceRef&, PerformOptions& perform)
    {
        perform.variables.try_emplace(0, 0);
    };
    profiled.arrangements = [](const ArchiveInput&, const ntdlib::ntr::SequenceRef&)
    {
        return std::vector<Arrangement>{{.suffix = "_PROGRAM7", .variables = {{0, 7}}}};
    };
    const auto convert =
        [&voices, &folder](const ArchiveConverter& c, const std::string& subfolder, const std::string& var = "")
    {
        std::vector<std::string> args = {kProgram, "--quiet", "--output",
                                         ntdlib::sequence_export::Utf8(folder.Path() / subfolder)};
        if (!var.empty())
        {
            args.insert(args.end(), {"--var", var});
        }
        args.push_back(ntdlib::sequence_export::Utf8(voices));
        return RunArchiveConverter(args, c);
    };
    // The samples of a SoundFont that the converter wrote: its smpl chunk.
    const auto samples = [&folder](const std::string& subfolder, const std::string& name)
    {
        const std::optional<std::vector<uint8_t>> sf2 =
            ntdlib::sequence_export::ReadFile(folder.Path() / subfolder / (name + ".sf2"));
        const std::string_view text(reinterpret_cast<const char*>(sf2 ? sf2->data() : nullptr), sf2 ? sf2->size() : 0);
        const std::size_t at = text.find("smpl");
        if (at == std::string_view::npos || at + 8 > text.size())
        {
            return std::string();
        }
        const uint32_t size = xsflib::LoadLe32(sf2->data() + at + 4);
        return std::string(text.substr(at + 8, size));
    };
    // The number of its samples: its shdr chunk's records but the last, which ends the list. A SoundFont without
    // samples still holds the 46 zero points that follow each sample.
    const auto sample_count = [&folder](const std::string& subfolder, const std::string& name)
    {
        const std::optional<std::vector<uint8_t>> sf2 =
            ntdlib::sequence_export::ReadFile(folder.Path() / subfolder / (name + ".sf2"));
        return sf2 ? seqlib::sf2::Read(*sf2).Count("shdr", 46) - 1 : 0;
    };
    Check(convert(converter, "plain") == 0 && convert(profiled, "profiled") == 0 &&
              convert(profiled, "given", "0=7") == 0 && convert(profiled, "below", "0=0") == 0,
          "the profiled archive converts");
    Check(passes(folder.Path() / "profiled" / "SE_VOICE") && passes(folder.Path() / "profiled" / "SE_VOICE_PROGRAM7") &&
              !fs::exists(folder.Path() / "plain" / "SE_VOICE_PROGRAM7.mid"),
          "an arrangement converts beside its sequence");
    Check(sample_count("plain", "SE_VOICE") == 0 && sample_count("profiled", "SE_VOICE") == 1 &&
              sample_count("profiled", "SE_VOICE_PROGRAM7") == 1 &&
              samples("profiled", "SE_VOICE") != samples("profiled", "SE_VOICE_PROGRAM7"),
          "the game's settings and the arrangement give their programs");
    Check(samples("given", "SE_VOICE") == samples("profiled", "SE_VOICE_PROGRAM7") &&
              samples("given", "SE_VOICE_PROGRAM7") == samples("profiled", "SE_VOICE_PROGRAM7") &&
              samples("below", "SE_VOICE") == samples("profiled", "SE_VOICE") &&
              samples("below", "SE_VOICE_PROGRAM7") == samples("profiled", "SE_VOICE"),
          "the command line's value stays over both");
    const std::optional<std::vector<uint8_t>> arranged =
        ntdlib::sequence_export::ReadFile(folder.Path() / "profiled" / "SE_VOICE_PROGRAM7.sf2");
    Check(arranged && std::string_view(reinterpret_cast<const char*>(arranged->data()), arranged->size())
                              .find("from sequence SE_VOICE_PROGRAM7.") != std::string_view::npos,
          "an arrangement's files take its name");

    // A game's profile with commands: its settings give track 0 a volume of 0 once the sequence starts, and an
    // arrangement gives it 127 again. The arrangement's commands follow the settings'.
    ArchiveConverter commanded = converter;
    commanded.game_settings = [](const ArchiveInput&, const ntdlib::ntr::SequenceRef&, PerformOptions& perform)
    {
        perform.commands.push_back(TrackVolume(0x0001, -32768));
    };
    commanded.arrangements = [](const ArchiveInput&, const ntdlib::ntr::SequenceRef&)
    {
        return std::vector<Arrangement>{{.suffix = "_FULL", .commands = {TrackVolume(0x0001, 0)}}};
    };
    const std::vector<std::string> args = {kProgram, "--quiet", "--output",
                                           ntdlib::sequence_export::Utf8(folder.Path() / "commanded"),
                                           ntdlib::sequence_export::Utf8(loose)};
    // The value of the first CC11 in a MIDI file that the converter wrote, or -1.
    const auto first_expression = [&folder](const std::string& name)
    {
        const std::optional<std::vector<uint8_t>> midi =
            ntdlib::sequence_export::ReadFile(folder.Path() / "commanded" / (name + ".mid"));
        if (!midi)
        {
            return -1;
        }
        for (const seqlib::midi::Track& track : seqlib::midi::Read(*midi).tracks)
        {
            for (const seqlib::midi::Event& e : track.events)
            {
                if (e.bytes.size() == 3 && (e.bytes[0] & 0xF0) == 0xB0 && e.bytes[1] == 11)
                {
                    return static_cast<int>(e.bytes[2]);
                }
            }
        }
        return -1;
    };
    Check(RunArchiveConverter(args, commanded) == 0 && first_expression("BGM_TEST") == 0 &&
              first_expression("BGM_TEST_FULL") == 107,
          "the game's settings' commands and an arrangement's after them");
}

} // namespace

int main()
{
    TestPanLaw();
    TestEnvelopes();
    TestPresetZones();
    TestWaves();
    TestPerform();
    TestChanges();
    TestStolenChannel();
    TestConvert();
    TestConvertBank();
    TestFrame();

    return xsflib::testing::Summary("DS conversion");
}
