// SPDX-License-Identifier: MIT

// Tests of the conversion of DSE's songs and sound effects (ntdlib/sequence_export/dse): the presets, the waves, the
// envelopes, the LFOs and the performance of a sequence, on the files of dse_song_files.h. The expected values are
// worked out by hand from the files and from the driver's model: its keys, its envelopes' levels and its timers.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/dse/sequence_input.h"
#include "ntdlib/sequence_export/psg_samples.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "sequence_export_dse_cases.h"

namespace
{

using namespace sequence_export_dse_cases;

// The zones of the bank's programs.
void TestPresetZones()
{
    const TestFiles test;
    const std::vector<PresetZone> one = PresetZones(test.bank, {.bank = files::kBankId, .program = 1});
    const std::vector<PresetZone> two = PresetZones(test.bank, {.bank = files::kBankId, .program = 2});
    Check(one.size() == 2 && two.size() == 2, "a zone for each split");
    if (one.size() != 2 || two.size() != 2)
    {
        return;
    }

    // Program 1's volume 0x70 times the split's 0x7F, over 127.
    const ntdlib::dse::EnvelopeValues unscaled = {.multiplier = 0,
                                                  .attack_begin = 0,
                                                  .attack_time = 0x08,
                                                  .decay_time = 0x0C,
                                                  .sustain_level = 0x50,
                                                  .hold_time = 0x04,
                                                  .sustain_time = 0x30,
                                                  .release_time = 0x20};
    Check(one[0].low_key == 0 && one[0].high_key == 63 && one[0].wave == 1 && one[0].format == 0 &&
              one[0].tuning == 100 && one[0].volume == 0x70 && one[0].pan == 0x20 && one[0].envelope == unscaled,
          "program 1's first split");
    // A coarse tuning of -1 adds -256 to the tuning of -300.
    Check(one[1].low_key == 48 && one[1].high_key == 127 && one[1].wave == 2 && one[1].format == 2 &&
              one[1].tuning == -556 && one[1].pan == 0x60,
          "program 1's second split, with its coarse tuning");
    Check(two[0].format == 3 && two[1].format == 4 && two[1].low_velocity == 0x20 && two[1].low_key == 64,
          "program 2's PSG and noise, from velocity 0x20");

    // A channel's channel_values takes the place of a split's value up to 0x7F.
    ntdlib::dse::EnvelopeValues channel_values;
    channel_values.attack_time = 0x7F;
    channel_values.release_time = 0x10;
    channel_values.sustain_level = 0x80;
    const std::vector<PresetZone> overridden =
        PresetZones(test.bank, {.bank = files::kBankId, .program = 1, .envelope = channel_values});
    Check(overridden.size() == 2 && overridden[0].envelope.attack_time == 0x7F &&
              overridden[0].envelope.release_time == 0x10 && overridden[0].envelope.sustain_level == 0x50 &&
              overridden[1].envelope.release_time == 0x10,
          "the channel's envelope values up to 0x7F take the splits' place");
    Check(PresetZones(test.bank, {.bank = files::kBankId, .program = 4}).empty() &&
              PresetZones(test.bank, {.bank = files::kBankId, .program = 200}).empty(),
          "a program that the bank lacks has no zones");

    // The oldest build plays a split's envelope multiplier of 16 or more as 1. The other builds keep it.
    ntdlib::dse::WaveBank steep = test.bank;
    steep.instruments->programs[1]->splits[0][ntdlib::dse::SplitView::kEnvelopeMultiplier] = 60;

    const std::vector<PresetZone> oldest =
        PresetZones(steep, {.bank = files::kBankId, .program = 1, .build = ntdlib::dse::Build::kOldest});
    const std::vector<PresetZone> later = PresetZones(steep, {.bank = files::kBankId, .program = 1});

    Check(oldest.size() == 2 && later.size() == 2 && oldest[0].envelope.multiplier == 1 &&
              later[0].envelope.multiplier == 60 && oldest[1].envelope == later[1].envelope,
          "the oldest build plays a multiplier of 60 as 1");
}

// The waves. A bank without samples plays its waves from the main bank's. The main bank's samples hold the bytes
// i * 7 and i * 13 + 5 for each i.
void TestWaves()
{
    const TestFiles test;
    BankSet banks(test.Song());
    const Pcm* pcm16 = banks.Wave(files::kBankId, 0);
    const Pcm* pcm8 = banks.Wave(files::kBankId, 1);
    const Pcm* adpcm = banks.Wave(files::kBankId, 2);
    const Pcm* psg = banks.Wave(files::kBankId, 3);
    const Pcm* noise = banks.Wave(files::kBankId, 4);
    const Pcm* lacking = banks.Wave(files::kBankId, 5);

    // 64 words of PCM16 from 0: 128 samples, looped from word 4, sample 8.
    Check(pcm16 != nullptr && pcm16->channels[0].size() == 128 && pcm16->channels[0][0] == 0x0500 &&
              pcm16->channels[0][1] == 0x1207 && pcm16->loop && pcm16->loop_start == 8 && pcm16->loop_end == 128 &&
              pcm16->sample_rate == 32000,
          "PCM16 from the main bank's samples, looped");
    // 32 words of PCM8 from 0x100: byte 0x100 is 128 * 7 = 0x380, and byte 0x101 is 128 * 13 + 5 = 0x685.
    Check(pcm8 != nullptr && pcm8->channels[0].size() == 128 && pcm8->channels[0][0] == -128 * 256 &&
              pcm8->channels[0][1] == -123 * 256 && !pcm8->loop && pcm8->sample_rate == 16000,
          "PCM8, once");
    // 32 words of IMA-ADPCM: a word of header and 248 samples, looped from word 9: sample (9 - 1) * 8.
    Check(adpcm != nullptr && adpcm->channels[0].size() == 248 && adpcm->loop && adpcm->loop_start == 64 &&
              adpcm->sample_rate == 22050,
          "IMA-ADPCM, looped after its header");
    Check(psg != nullptr && psg->channels[0] == ntdlib::sequence_export::PsgSamples(3) && psg->loop &&
              psg->loop_start == 0 && psg->loop_end == 128 &&
              psg->sample_rate == ntdlib::sequence_export::kPsgSampleRate,
          "the PSG at the wave's duty of 3");
    Check(noise != nullptr && noise->channels[0] == ntdlib::sequence_export::NoiseSamples() && noise->loop,
          "the noise");
    Check(lacking == nullptr &&
              banks.Warnings() == std::vector<std::string>{"w32512_5: no bank holds the wave's samples"},
          "a wave that the main bank lacks gives a warning");
    Check(BankSet::WaveName(files::kBankId, 1) == "w32512_1", "the waves' names");
}

// The envelopes' fits to the driver's levels. A voice plays the square of its envelope's level over 127.
void TestEnvelopes()
{
    // The splits' default: attack, hold and decay of 0 start at full level, and a sustain time of 0x7F holds it.
    const ntdlib::dse::EnvelopeValues held = {.multiplier = 1,
                                              .attack_begin = 0,
                                              .attack_time = 0,
                                              .decay_time = 0,
                                              .sustain_level = 0x7F,
                                              .hold_time = 0,
                                              .sustain_time = 0x7F,
                                              .release_time = 0x28};
    const std::vector<uint8_t> levels = ntdlib::dse::EnvelopeLevels(held, kTenMs, 100);
    const Sf2Envelope sf2 = ConvertEnvelope(held, kTenMs);
    Check(std::ranges::all_of(levels, [](uint8_t level) { return level == 127; }) && sf2.attack == -12000 &&
              sf2.sustain == 0,
          "an envelope at full level from its start");

    // The driver's release falls linearly in level to 0 in its ticks. At half of them, the voice plays (1/2)^2:
    // -12.0 dB. SoundFont's release falls 100 dB in its time.
    const std::optional<int32_t> ticks = ntdlib::dse::EnvelopeTicks(1, 0x28, kTenMs);
    const double release_ms = std::pow(2.0, sf2.release / 1200.0) * 1000.0;
    const double half_db = ticks ? -100.0 * (*ticks * kTickMs / 2.0) / release_ms : 0.0;
    Check(ticks && std::abs(half_db + 12.04) < 0.5,
          "the release passes the driver's level at half its time within 0.5 dB: " + std::to_string(half_db));
    // The driver's level falls by 127 / ticks in each tick of the release, from the first.
    const std::vector<uint8_t> released = ntdlib::dse::EnvelopeLevels(held, kTenMs, *ticks + 2, 0);
    Check(released.size() == static_cast<std::size_t>(*ticks + 2) && released.back() == 0 &&
              std::abs(released[static_cast<std::size_t>(*ticks / 2 - 1)] - 63.5) <= 1.0,
          "the driver's release falls linearly");

    // Program 0's split: an attack from 0x20 and a decay to a sustain of 0x60. The sustain plays 40 * log10(96 / 127) =
    // -4.86 dB: 48.6 cB, and (96 / 127)^2 = 57.1 % of full amplitude.
    const ntdlib::dse::EnvelopeValues slow = {.multiplier = 1,
                                              .attack_begin = 0x20,
                                              .attack_time = 0x10,
                                              .decay_time = 0x20,
                                              .sustain_level = 0x60,
                                              .hold_time = 0,
                                              .sustain_time = 0x7F,
                                              .release_time = 0x28};
    const Sf2Envelope slow_sf2 = ConvertEnvelope(slow, kTenMs);
    const SfzEnvelope slow_sfz = ConvertEnvelopeForSfz(slow, kTenMs);
    Check(slow_sf2.sustain == 49 && std::abs(slow_sfz.sustain - 57.14) < 0.01, "the sustain's level");
    // The decay starts where the driver's does: the attack of 0x10 takes a tick and the level holds for another, then
    // the decay of 0x20 takes 3 ticks to the sustain.
    const std::vector<uint8_t> fall = ntdlib::dse::EnvelopeLevels(slow, kTenMs, 8);
    const double decay_starts_ms = (slow_sfz.attack + slow_sfz.hold) * 1000.0;
    Check(ntdlib::dse::EnvelopeTicks(1, 0x10, kTenMs) == 1 && ntdlib::dse::EnvelopeTicks(1, 0x20, kTenMs) == 3 &&
              fall == std::vector<uint8_t>{127, 127, 116, 106, 96, 96, 96, 96} &&
              std::abs(decay_starts_ms - 2.0 * kTickMs) < 1e-6,
          "the decay starts after two ticks at full level: " + std::to_string(decay_starts_ms) + " ms");
    // SoundFont's decay falls 100 dB in its time. It reaches the sustain's 4.86 dB after about 3 ticks.
    const double sustain_ms = std::pow(2.0, slow_sf2.decay / 1200.0) * 1000.0 * 4.862 / 100.0;
    Check(std::abs(sustain_ms - 3.0 * kTickMs) < kTickMs,
          "the decay reaches the sustain within a tick of the driver's: " + std::to_string(sustain_ms) + " ms");

    // No attack, no hold and no decay: the level falls from full, from the first tick, linearly to 0 in the ticks of
    // the sustain's time. At half of those ticks, the voice plays (1/2)^2: -12.0 dB. SoundFont's decay falls 100 dB in
    // its time.
    const ntdlib::dse::EnvelopeValues fading = {.multiplier = 1,
                                                .attack_begin = 0,
                                                .attack_time = 0,
                                                .decay_time = 0,
                                                .sustain_level = 0x7F,
                                                .hold_time = 0,
                                                .sustain_time = 0x60,
                                                .release_time = 0x28};
    const std::optional<int32_t> fading_ticks = ntdlib::dse::EnvelopeTicks(1, 0x60, kTenMs);
    const std::vector<uint8_t> fading_levels = ntdlib::dse::EnvelopeLevels(fading, kTenMs, 1);
    Check(fading_ticks && *fading_ticks > 254 && fading_levels[0] == 126, "the level falls from the first tick");
    const Sf2Envelope fading_sf2 = ConvertEnvelope(fading, kTenMs);
    const SfzEnvelope fading_sfz = ConvertEnvelopeForSfz(fading, kTenMs);
    const double half_ms = fading_ticks ? *fading_ticks * kTickMs / 2.0 : 0.0;
    const double fading_db =
        -100.0 * (half_ms - fading_sfz.hold * 1000.0) / (std::pow(2.0, fading_sf2.decay / 1200.0) * 1000.0);
    Check(fading_sf2.sustain == 1440 && std::abs(fading_db + 12.04) < 3.0,
          "a fall from the first tick passes the driver's level at half its time within 3 dB: " +
              std::to_string(fading_db));
}

// The tick of 16.715 ms: each of the driver's stages takes fewer ticks, each a video frame long, and the envelopes'
// times follow the driver's.
void TestSlowTickEnvelopes()
{
    constexpr int kSlow = ntdlib::dse::Driver::kSlowTickMicroseconds;
    const ntdlib::dse::EnvelopeValues e = {.multiplier = 1,
                                           .attack_begin = 0,
                                           .attack_time = 0x30,
                                           .decay_time = 0x40,
                                           .sustain_level = 0x40,
                                           .hold_time = 0,
                                           .sustain_time = 0x7F,
                                           .release_time = 0x50};
    const std::optional<int32_t> ten = ntdlib::dse::EnvelopeTicks(1, 0x50, kTenMs);
    const std::optional<int32_t> slow = ntdlib::dse::EnvelopeTicks(1, 0x50, kSlow);
    Check(ten && slow && std::abs(*slow - *ten * 10000.0 / kSlow) <= 1.0, "a stage's ticks follow the tick");

    constexpr double kFrameMs = 1000.0 * 560190 / 33513982.0;
    const Sf2Envelope a = ConvertEnvelope(e, kTenMs);
    const Sf2Envelope b = ConvertEnvelope(e, kSlow);
    const auto ms = [](int timecents)
    {
        return std::pow(2.0, timecents / 1200.0) * 1000.0;
    };
    // The driver's release lasts the same number of ticks in either tick. The release's fit is the same against it.
    const double ten_ratio = ms(a.release) / (*ten * kTickMs);
    const double slow_ratio = ms(b.release) / (*slow * kFrameMs);
    Check(std::abs(slow_ratio / ten_ratio - 1.0) < 0.002 && a.release != b.release,
          "the release follows the driver's release in frames: " + std::to_string(ms(a.release)) + " and " +
              std::to_string(ms(b.release)) + " ms");
    Check(std::abs(ms(a.attack) - ms(b.attack)) < 2.0 * kFrameMs, "the attack keeps its time within two frames");
    Check(a.sustain == b.sustain, "the sustain doesn't depend on the tick");
    const SfzEnvelope sfz = ConvertEnvelopeForSfz(e, kSlow);
    Check(std::abs(sfz.release * 1000.0 - ms(b.release) * 0.6949) < 1.0, "SFZ's release from the same fit");
}

// A preset's LFO with a phase of 50 ms runs whole ticks, like the driver's: 5 ticks of 10 ms, or 2 of 16.715 ms. A
// phase shorter than a tick takes one tick. Waveform 0 takes two phases for a cycle.
void TestLfoTicks()
{
    PresetKey key;
    key.lfos[0] = {.type = 1, .output = 1, .waveform = 0, .amplitude = 100, .phase_ms = 50};
    key.lfos[1] = {.type = 1, .output = 1, .waveform = 0, .amplitude = 100, .phase_ms = 5};
    const std::vector<LfoShape> ten = PresetLfos(key, ntdlib::dse::Driver::kTickMicroseconds);
    const std::vector<LfoShape> slow = PresetLfos(key, ntdlib::dse::Driver::kSlowTickMicroseconds);
    Check(ten.size() == 2 && slow.size() == 2, "both LFOs");
    Check(ten.size() == 2 && ten[0].hz == 10.0 && ten[1].hz == 50.0, "10 ms: 5 ticks a phase, and one tick");
    Check(slow.size() == 2 && std::abs(slow[0].hz - 1e6 / (2.0 * 2 * 16'715)) < 1e-9 &&
              std::abs(slow[1].hz - 1e6 / (2.0 * 16'715)) < 1e-9,
          "16.715 ms: 2 ticks a phase, and one tick");

    // The oldest build's waveform 3 swings over twice the amplitude in four phases, the other builds' over the
    // amplitude in two.
    PresetKey triangle;
    triangle.lfos[0] = {.type = 1, .output = 1, .waveform = 3, .amplitude = 100, .phase_ms = 50};

    const std::vector<LfoShape> later = PresetLfos(triangle, ntdlib::dse::Driver::kTickMicroseconds);
    triangle.build = ntdlib::dse::Build::kOldest;
    const std::vector<LfoShape> oldest = PresetLfos(triangle, ntdlib::dse::Driver::kTickMicroseconds);

    Check(later.size() == 1 && oldest.size() == 1 && later[0].hz == 10.0 && oldest[0].hz == 5.0 &&
              oldest[0].depth == 2 * later[0].depth,
          "the oldest build's waveform 3: half the rate and twice the depth");
}

// The performer. Track 4 plays keys 57, 53 and 50 (octave 4 and keys 9, 5 and 2) at velocity 127, 16 ticks apart, on
// MIDI track 4. Tracks 0 to 3 loop back to their loops' starts. The settings give 48 ticks to a quarter note.
void TestPerform()
{
    const TestFiles test;
    const Performance p = Perform(test.Song());
    const std::vector<TrackEvent> ons = EventsOf(p, 4, EventKind::kNoteOn);
    std::vector<std::pair<uint32_t, int>> keys;
    for (const TrackEvent& e : ons)
    {
        keys.emplace_back(e.tick, e.key);
    }
    Check(keys == std::vector<std::pair<uint32_t, int>>{{0, 57}, {16, 53}, {32, 50}} &&
              std::ranges::all_of(ons, [](const TrackEvent& e) { return e.value == 127; }),
          "track 4's notes, keys and velocities");
    const std::vector<TrackEvent> reverb = EventsOf(p, 4, EventKind::kReverb);
    Check(!reverb.empty() && reverb[0].tick == 0 &&
              std::ranges::all_of(reverb, [](const TrackEvent& e) { return e.value == 0; }),
          "the driver has no reverb send: CC91 0 from the start");
    Check(std::ranges::all_of(p.tracks, ntdlib::testing::ReverbBeforeNotes),
          "every part: CC91 before its first note-on");
    Check(p.timebase == 48 && !p.tempo.empty(), "the timebase and the tempo");
    Check(p.loop.has_value() && !p.truncated, "the song loops");
    Check(!p.presets.empty() &&
              std::ranges::all_of(p.presets, [](const PresetKey& key) { return key.bank == files::kBankId; }),
          "the presets of the song's bank");

    // A sound effect plays on the first sequence of the pool. Effect 1 plays keys 48 and 52 at velocity 0x60, 24
    // ticks apart, and ends.
    const ntdlib::dse::SoundEffects effects = ntdlib::dse::SoundEffects::Parse(EffectsFile());
    SequenceFiles effect = test.Song();
    effect.song = nullptr;
    effect.effects = &effects;
    effect.effect = 1;
    const Performance fx = Perform(effect);
    const std::vector<TrackEvent> fx_ons = EventsOf(fx, 0, EventKind::kNoteOn);
    Check(fx_ons.size() == 2 && fx_ons[0].tick == 0 && fx_ons[0].key == 48 && fx_ons[0].value == 0x60 &&
              fx_ons[1].tick == 24 && fx_ons[1].key == 52 && !fx.loop && !fx.truncated,
          "a sound effect's notes");

    // An effect whose track ends more than 10 s after its last sound has died away: the two notes, a wait of 2000
    // ticks (20 s) and the end. The performance ends where the sound dies away. After a wait of 900 ticks (9 s), it
    // ends with the track.
    const auto ending = [](uint16_t wait)
    {
        Bytes track =
            files::Join({{1, 0, 0, 0},
                         {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                         files::Note(0x60, 0, 0, 0x18, 1),
                         {0x92, 0x18},
                         files::Note(0x60, 4, 0),
                         {0x92, 0x18, 0x93, static_cast<uint8_t>(wait), static_cast<uint8_t>(wait >> 8), 0x98}});
        return ntdlib::dse::SoundEffects::Parse(files::EffectsFile({files::Effect{.tracks = {track}}}, 5));
    };
    const ntdlib::dse::SoundEffects late_effects = ending(2000);
    const ntdlib::dse::SoundEffects in_time_effects = ending(900);
    effect.effect = 0;
    effect.effects = &late_effects;
    const Performance late = Perform(effect);
    effect.effects = &in_time_effects;
    const Performance in_time = Perform(effect);
    Check(EventsOf(late, 0, EventKind::kNoteOn).size() == 2 && late.end_tick >= 48 && late.end_tick < 300 &&
              late.seconds < 3.0 && !late.truncated,
          "a track's end long after the last sound: the performance ends at the sound");
    Check(in_time.end_tick == 948 && !in_time.truncated, "a track's end within 10 s of it: the performance ends there");

    effect.effects = &effects;
    effect.effect = 0;
    const Performance refused = Perform(effect);
    Check(EventsOf(refused, 0, EventKind::kNoteOn).empty() &&
              refused.warnings == std::vector<std::string>{"the driver refuses the sound effect"},
          "an empty slot plays nothing");
}

// A song whose track 1 holds a note of 3000 ticks and then loops 50 ticks of waits, and whose track 2 plays a note,
// waits 1100 ticks, plays another note, waits `last` ticks and ends, without a loop. Track 1's passes repeat from the
// start, but track 2 hasn't gone round a loop before it ends. The song then holds track 1's note. It ends 10 s after
// its last change, the note at tick 1100. At 120 beats a minute and 48 ticks a beat, a tick takes 10.4167 ms of the
// driver's nominal time, and 10.4156 ms of the time that kTickMs gives. 10 s therefore comes 960.09 ticks after tick
// 1100, in tick 2061. A wait of 50 ticks ends track 2 in the tick that ends one of track 1's passes.
// With the tick of 16.715 ms, the driver's ticks come once a video frame, and a sequence's ticks keep their times: the
// MIDI tempo follows the driver's tick against its nominal length.
void TestSlowTick()
{
    const TestFiles test;
    const Performance ten = Perform(test.Song());
    const Performance slow = Perform(test.Song(), {.tick_microseconds = ntdlib::dse::Driver::kSlowTickMicroseconds});
    constexpr double kFrameMs = 1000.0 * 560190 / 33513982.0;
    Check(ten.tick_microseconds == 10000 && slow.tick_microseconds == 16715, "the performance keeps its tick");
    Check(EventsOf(slow, 4, EventKind::kNoteOn).size() == EventsOf(ten, 4, EventKind::kNoteOn).size() &&
              EventsOf(slow, 4, EventKind::kNoteOn)[1].tick == 16,
          "the notes keep their ticks");
    // A tick of the sequence takes tempo / timebase microseconds of the driver's nominal ticks.
    const double ten_bpm = ten.tempo.at(0).bpm * kTickMs / 10.0;
    const double slow_bpm = slow.tempo.at(0).bpm * kFrameMs / 16.715;
    Check(std::abs(ten_bpm - slow_bpm) < 1e-9, "the tempo follows the tick: " + std::to_string(ten.tempo[0].bpm) +
                                                   " and " + std::to_string(slow.tempo[0].bpm));
    Check(std::abs(ten.seconds - slow.seconds) < 2.0 * kFrameMs / 1000.0,
          "the length keeps its time within two frames: " + std::to_string(ten.seconds) + " and " +
              std::to_string(slow.seconds) + " s");
}

void TestHeldSound()
{
    const TestFiles test;
    const Bytes select = {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40};
    for (const uint8_t last : {uint8_t{0x18}, uint8_t{0x32}})
    {
        const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(files::SongFile(
            {files::Join({{0, 0, 0, 0, 0xA4, 0x78, 0x98}}),
             files::Join({{1, 1, 0, 0}, select, files::Note(0x60, 0, 0, 3000, 2), {0x99, 0x92, 0x32, 0x98}}),
             files::Join({{2, 2, 0, 0},
                          select,
                          files::Note(0x60, 4, 0, 0x18, 1),
                          {0x93, 0x4C, 0x04},
                          files::Note(0x60, 7, 0),
                          {0x92, last, 0x98}})}));
        SequenceFiles files = test.Song();
        files.song = &song;
        const Performance p = Perform(files);
        std::vector<uint32_t> ticks;
        for (const TrackEvent& e : EventsOf(p, 2, EventKind::kNoteOn))
        {
            ticks.push_back(e.tick);
        }
        const std::string wait = " (a last wait of " + std::to_string(last) + " ticks)";
        Check(ticks == std::vector<uint32_t>{0, 1100},
              "a track without a loop plays on while another holds a note" + wait);
        Check(p.holds && !p.loop && p.end_tick == 2061 && !p.truncated,
              "the held note ends the song 10 s after the last change" + wait);
    }
}

// The conversions of the song and of the sound effect.
void TestConvert()
{
    const TestFiles test;
    const Conversion c = ConvertSequence(test.Song(), "song", {}, kProgram);
    CheckFiles(c, "the song");
    // Program 3 plays wave 5. The main bank has no wave 5.
    Check(c.warnings == std::vector<std::string>{"w32512_5: no bank holds the wave's samples"},
          "a warning for the wave without samples alone");

    // The PSG's timer steps it once a period, 16 samples of its wave: the zone plays at a PCM wave's rate of the same
    // timer, 16 times the step's rate.
    const std::vector<PresetZone> two = PresetZones(test.bank, {.bank = files::kBankId, .program = 2});
    if (two.size() == 2)
    {
        PresetZone pcm = two[0];
        pcm.format = 0;
        const ZoneTuning psg_tuning = TuningOf(two[0], ntdlib::sequence_export::kPsgSampleRate);
        const ZoneTuning pcm_tuning = TuningOf(pcm, ntdlib::sequence_export::kPsgSampleRate);
        Check(std::abs((psg_tuning.coarse * 100 + psg_tuning.fine) - (pcm_tuning.coarse * 100 + pcm_tuning.fine)) <= 1,
              "the PSG's tuning");
    }

    // Program 1's first zone: a volume of 112 and a pan of 0x20. (112 / 127)^2 = 0.77775, and the pan's gain against
    // the centre's is hypot(32, 96) / hypot(64, 64) = 1.11803.
    const std::vector<PresetZone> one = PresetZones(test.bank, {.bank = files::kBankId, .program = 1});
    Check(!one.empty() && std::abs(ZoneLevel(one[0]) - 0.86955) < 1e-4, "a zone's level with its pan");

    const ntdlib::dse::SoundEffects effects = ntdlib::dse::SoundEffects::Parse(EffectsFile());
    SequenceFiles effect = test.Song();
    effect.song = nullptr;
    effect.effects = &effects;
    effect.effect = 1;
    const Conversion fx = ConvertSequence(effect, "fx_001", {}, kProgram);
    CheckFiles(fx, "the sound effect");
    const std::string sf2(fx.sf2.begin(), fx.sf2.end());
    Check(sf2.find("Converted by xyz2midi from sound effect fx_001.") != std::string::npos, "the SoundFont's comment");
}

// A note that another note cuts rings on in its release where another note sounds on its part. Key 60 of program 0
// sounds from tick 0 to 96. Keys 64, 65 and 67 of program 1 start at ticks 0, 6 and 12, on the split whose wave loops.
// Key group 1 allows a channel two voices: key 67 takes the voice of key 64 or key 65. Their release of 8 ticks
// counts. A release of no ticks doesn't.
void TestCuts()
{
    const Bytes track = files::Join({{1, 0, 0, 0},
                                     {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                                     files::Note(0x60, 0, 1, 0x60, 1),
                                     {0xAC, 0x01},
                                     files::Note(0x60, 4, 0, 0x30, 1),
                                     {0x92, 0x06},
                                     files::Note(0x60, 5, 0),
                                     {0x92, 0x06},
                                     files::Note(0x60, 7, 0, 0x0C, 1),
                                     {0x92, 0x60, 0x98}});
    TestFiles test;
    const Conversion c = ConvertEffect(test, track);
    CheckFiles(c, "the cut");
    const std::vector<std::pair<uint32_t, int>> offs = NoteOffs(c.performance, 0);
    Check(std::ranges::count_if(offs, [](const auto& off) { return off.first == 12; }) == 1,
          "key 67 cuts a note at tick 12");
    Check(c.performance.approximations.contains(kCut) && c.performance.approximations.at(kCut) == 1,
          "the cut rings on in its release");

    test.bank.instruments->programs.at(1)->splits.at(1)[0x2E] = 0;
    const Conversion quick = ConvertEffect(test, track);
    Check(NoteOffs(quick.performance, 0) == offs && !quick.performance.approximations.contains(kCut),
          "a release of no ticks doesn't count");
}

// The ticks of the driver from the start of the song or the sound effect of `files`, `ticks` of them, with whether
// dse::Player's render is silent in each: every sample at the bias.
std::vector<bool> SilentTicks(const SequenceFiles& files, int ticks)
{
    ntdlib::dse::Player player;
    player.LoadBanks(files);
    player.Start(files);

    std::vector<bool> silent;
    std::vector<ntdlib::ntr::SoundOutput> out;
    for (int t = 0; t < ticks; t++)
    {
        out.clear();
        player.RunTick(out);
        const auto at_bias = [](const ntdlib::ntr::SoundOutput& s)
        {
            return s.left == 0x200 && s.right == 0x200;
        };
        silent.push_back(std::ranges::all_of(out, at_bias));
    }

    return silent;
}

// The ticks of the driver from a sound effect's start (SilentTicks).
std::vector<bool> SilentTicks(const TestFiles& test, const ntdlib::dse::SoundEffects& effects, int ticks)
{
    SequenceFiles effect = test.Song();
    effect.song = nullptr;
    effect.effects = &effects;
    return SilentTicks(effect, ticks);
}

// The driver's tick in which an event of `p` at `tick` and `after` plays, at the performance's first tempo.
int DriverTickOf(const Performance& p, uint32_t tick, double after)
{
    const double ms_per_tick = 60000.0 / p.tempo.at(0).bpm / p.timebase;
    return static_cast<int>(std::lround((tick + after) * ms_per_tick / kTickMs));
}

// A voice whose hardware volume is 0 is silent on the console, whatever its envelope's level. Program 0's split plays
// at 110 of 127, and its envelope rises from 0x20 to 127 and decays to 0x60. dse::Player's render anchors both cases.
//
// A note held at a channel volume of 12 is silent. The driver's quotient of the volumes, 12 * 110 * 127 / 8032, is
// under 23. The volume rises to 127 at tick 48, and the note sounds from there. Its part takes a CC11 of 0 until then.
// The note's voice ends after its release, and only there may CC120 stop the part.
//
// A note at a channel volume of 64 releases from tick 24. Its voice falls silent where the quotient 64 * 110 * level /
// 8032 drops under 23, at a level of 26, while the envelope still has about a fifth of its release to go. CC120 stops
// the part there. MIDI's release would ring on.
void TestSilentVoices()
{
    const TestFiles test;
    const auto effects_of = [](const Bytes& track)
    {
        return ntdlib::dse::SoundEffects::Parse(files::EffectsFile({files::Effect{.tracks = {track}}}, 5));
    };
    const ntdlib::dse::SoundEffects held = effects_of(files::Join({{1, 0, 0, 0},
                                                                   {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x0C},
                                                                   files::Note(0x7F, 0, 0, 0x60, 1),
                                                                   {0x92, 0x30, 0xE0, 0x7F, 0x92, 0x30, 0x98}}));
    const ntdlib::dse::SoundEffects released = effects_of(files::Join({{1, 0, 0, 0},
                                                                       {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x40},
                                                                       files::Note(0x7F, 0, 0, 0x18, 1),
                                                                       {0x92, 0x18, 0x93, 0x90, 0x01, 0x98}}));

    // The anchor: the held note is silent until the volume rises at tick 48, the driver's tick 50, and sounds after.
    const std::vector<bool> held_silent = SilentTicks(test, held, 90);
    Check(std::all_of(held_silent.begin(), held_silent.begin() + 49, [](bool s) { return s; }) &&
              std::none_of(held_silent.begin() + 52, held_silent.end(), [](bool s) { return s; }),
          "dse::Player: the quiet note is silent until its volume rises");

    SequenceFiles effect = test.Song();
    effect.song = nullptr;
    effect.effects = &held;
    const Performance p = Perform(effect);
    const std::vector<TrackEvent> expression = EventsOf(p, 0, EventKind::kExpression);
    const auto before_rise = [](const TrackEvent& e)
    {
        return e.tick < 48;
    };
    const auto sounding = [](const TrackEvent& e)
    {
        return e.tick == 48 && e.value > 0;
    };
    Check(!expression.empty() && expression.front().tick == 0 && expression.front().value == 0 &&
              std::ranges::all_of(expression | std::views::filter(before_rise),
                                  [](const auto& e) { return e.value == 0; }) &&
              std::ranges::any_of(expression, sounding),
          "the quiet note's part takes a CC11 of 0 until the volume rises");
    const std::vector<TrackEvent> note_offs = EventsOf(p, 0, EventKind::kNoteOff);
    Check(EventsOf(p, 0, EventKind::kNoteOn).size() == 1 && note_offs.size() == 1 &&
              std::ranges::all_of(EventsOf(p, 0, EventKind::kSoundOff),
                                  [&](const TrackEvent& e) { return e.tick >= note_offs[0].tick; }),
          "the quiet note stays one MIDI note, and no CC120 cuts it before its end");

    // The anchor of the release: the driver's ticks from the first that sounds to the first from which the render
    // stays silent. The commands of a tick reach the sound unit in the ARM7's next frame, for the note's start and for
    // its stop alike. The MIDI file starts the note at the start of the driver's tick that starts its voice.
    const std::vector<bool> released_silent = SilentTicks(test, released, 400);
    const auto first_sound = std::ranges::find(released_silent, false);
    int silent_from = static_cast<int>(released_silent.size());
    while (silent_from > 0 && released_silent[static_cast<std::size_t>(silent_from - 1)])
    {
        silent_from--;
    }
    const int sounding_ticks = silent_from - static_cast<int>(first_sound - released_silent.begin());
    Check(sounding_ticks > 25 && sounding_ticks < 350, "dse::Player: the release falls silent");

    effect.effects = &released;
    const Performance r = Perform(effect);
    const std::vector<TrackEvent> stops = EventsOf(r, 0, EventKind::kSoundOff);
    Check(stops.size() == 1 && std::abs(DriverTickOf(r, stops[0].tick, stops[0].after) - sounding_ticks) <= 1,
          "CC120 stops the release where its voice falls silent");
}

// A loop whose passes start silent. Track 1 loops from tick 0: an expression of 12, key 0 for 96 ticks, a wait of 48,
// an expression of 127, a wait of 24, an expression of 12 and a wait of 24. Program 0 plays at 110, and the driver's
// quotient of the volumes, 12 * 110 * 127 / 8032, is under 23: the note is silent until tick 48 and from tick 72 of
// each pass. The first pass's note starts on a part that sounded nothing before. The later passes' notes start on a
// part that is muted already, under the held note of the pass before. The passes play alike, and the loop is the first
// pass. dse::Player's render anchors the silence: every pass is silent until its expression rises, and sounds after.
void TestLoopOverAMute()
{
    const TestFiles test;
    const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(
        files::SongFile({files::Join({{0, 0, 0, 0, 0xA4, 0x78, 0x98}}),
                         files::Join({{1, 1, 0, 0},
                                      {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40, 0x99, 0xE3, 0x0C},
                                      files::Note(0x7F, 0, 0, 0x60, 1),
                                      {0x92, 0x30, 0xE3, 0x7F, 0x92, 0x18, 0xE3, 0x0C, 0x92, 0x18, 0x98}})}));
    SequenceFiles files = test.Song();
    files.song = &song;

    // The anchor. A pass lasts 100 of the driver's ticks, and a tick's commands sound 4 of them later.
    const std::vector<bool> silent = SilentTicks(files, 300);
    const auto all_silent = [&](int from, int to)
    {
        return std::all_of(silent.begin() + from, silent.begin() + to, [](bool s) { return s; });
    };
    const auto all_sound = [&](int from, int to)
    {
        return std::none_of(silent.begin() + from, silent.begin() + to, [](bool s) { return s; });
    };
    Check(all_silent(0, 50) && all_sound(54, 76) && all_silent(80, 150) && all_sound(154, 176) && all_silent(180, 250),
          "dse::Player: each pass is silent until its expression rises");

    const Performance p = Perform(files);
    const std::vector<TrackEvent> expression = EventsOf(p, 1, EventKind::kExpression);
    Check(std::ranges::any_of(expression, [](const TrackEvent& e) { return e.value == 0; }),
          "the silent note's part takes a CC11 of 0");
    Check(p.loop && p.loop->start == 0 && p.loop->end == 96, "a loop whose passes start silent: the first pass");
}

} // namespace

int main()
{
    TestPresetZones();
    TestWaves();
    TestEnvelopes();
    TestSlowTickEnvelopes();
    TestLfoTicks();
    TestPerform();
    TestSlowTick();
    TestHeldSound();
    TestConvert();
    TestCuts();
    TestSilentVoices();
    TestLoopOverAMute();

    return xsflib::testing::Summary("DSE conversion");
}
