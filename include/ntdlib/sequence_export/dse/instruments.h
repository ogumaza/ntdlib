// SPDX-License-Identifier: MIT

// The programs of DSE's wave banks as the zones of SoundFont and SFZ presets, the waves that their splits play, and
// DSE's envelopes and LFOs in SoundFont's and SFZ's terms.
//
// A split of a program plays a wave of its bank for the keys and velocities in its ranges. A note takes a voice for
// each split that holds its key and velocity, and so does a SoundFont or SFZ note for each zone or region. A voice
// plays its wave at the rate of its pitch: the driver's table gives each pitch a rate, and the split's tuning moves the
// key's pitch. A bank's waves can be in the main bank's samples. The sound unit plays the waves without interpolation.
//
// A voice's volume is the square of the product of the channel's volumes, the note's level and the envelope's level.
// The note's level is the product of the velocity, the program's volume and the split's volume. The envelope's level
// moves linearly from 0 to 127 in each stage: an attack from its start level, a hold, a decay to the sustain level, a
// fade of the sustain to 0 or a sustain that holds, and a release. Its square rises and falls slowly near full level,
// and quickly near silence. SoundFont's and SFZ's attacks rise linearly in amplitude, and their decays and releases
// fall linearly in dB. The times of each come from fits to the driver's levels.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/pcm.h"
#include "seqlib/sf2/units.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/sequence_export/dse/performance.h"

namespace ntdlib::sequence_export::dse
{

// A decoded wave, and its rate in the bank.
struct Pcm : seqlib::Pcm
{
    uint32_t sample_rate = 0;
};

// A zone of a preset: a split of the program, with the channel's envelope over the split's.
struct PresetZone
{
    uint8_t low_key = 0;
    uint8_t high_key = 127;
    uint8_t low_velocity = 0;
    uint8_t high_velocity = 127;
    uint16_t wave = 0;  // the wave's slot in the bank
    uint8_t format = 0; // the wave's byte 0x13: 0 PCM8, 1 PCM16 and 2 IMA-ADPCM
    int16_t tuning = 0; // the split's change of the key's pitch, in 256ths of a semitone
    uint8_t volume = 0; // the product of the program's and the split's volumes, over 127
    uint8_t pan = 0x40; // the split's pan
    ntdlib::dse::EnvelopeValues envelope;

    // The split's key group where the group allows a channel one voice, or 0. A note of such a group takes the voice
    // of the group's last note on the channel. A SoundFont note of the exclusive class cuts the other notes of the
    // class on its MIDI channel, and so does an SFZ note of the group (group and off_by).
    uint8_t exclusive_class = 0;
};

// The zones of `key`'s program in `bank`, one for each split in the order of the driver's search. A split whose wave is
// missing from the bank has no zone.
std::vector<PresetZone> PresetZones(const ntdlib::dse::WaveBank& bank, const PresetKey& key);

// The volume envelope in SoundFont's terms: times in timecents, and the sustain in centibels of attenuation. The decay
// and the release times give a fall of 100 dB.
struct Sf2Envelope
{
    int attack = seqlib::sf2::kShortestTimecents, hold = seqlib::sf2::kShortestTimecents,
        decay = seqlib::sf2::kShortestTimecents, sustain = 0, release = seqlib::sf2::kShortestTimecents;
};

// The envelope of `e` as the driver steps it with a tick of `tick_microseconds` (dse::PlayerOptions).
Sf2Envelope ConvertEnvelope(const ntdlib::dse::EnvelopeValues& e, int tick_microseconds);

// The amplitude envelope in SFZ's terms: times in seconds, and the sustain in percent of full amplitude. The decay and
// the release fall by a factor of e^-8 (69.49 dB) over their time. sfzformat.com's ampeg_attack page gives that fall.
struct SfzEnvelope
{
    double attack = 0.0, hold = 0.0, decay = 0.0, sustain = 100.0, release = 0.0;
};

// The same in SFZ's terms.
SfzEnvelope ConvertEnvelopeForSfz(const ntdlib::dse::EnvelopeValues& e, int tick_microseconds);

// The outputs of an LFO (LfoValues::output and LfoShape::output).
constexpr uint8_t kPitchOutput = 1;
constexpr uint8_t kVolumeOutput = 2;
constexpr uint8_t kPanOutput = 3;

// An LFO of a preset in the units that SoundFont and SFZ take: its rate in Hz, its delay and fade in seconds, and half
// its swing: in cents of pitch, in steps of the note's level from 0 to 127, or in steps of the pan from 0 to 127.
// `offset` is the middle of the swing in the same units. It is 0 for a waveform that swings around 0, and `depth` or
// -`depth` for one that swings from 0 to one side. `triangle` is set when the driver's waveform is the triangle that
// swings from 0 up to the top first and around 0, as SoundFont's LFOs do.
struct LfoShape
{
    uint8_t output = 0; // 1 the pitch, 2 the volume and 3 the pan
    uint8_t waveform = 0;
    double hz = 0.0;
    double delay = 0.0;
    double fade = 0.0;
    double depth = 0.0;
    bool triangle = false;
    double offset = 0.0;
};

// The preset's LFOs that change the sound, in the order of their slots, with a tick of `tick_microseconds` in the
// game's settings.
std::vector<LfoShape> PresetLfos(const PresetKey& key, int tick_microseconds);

// A volume LFO in dB, at the level of the zone's notes at a velocity of 127. The driver adds the LFO to the note's
// level and holds the sum from 0 to 127 before it squares it (0x02074194). `shift` is the change of the zone's level
// and `swing` half the LFO's swing in dB, with the sign of the LFO's depth: the straight line in dB that fits the
// driver's level best. `power_mean` is the mean power of the driver's level against the zone's in dB: 10 log10 of the
// mean of (held / level)^4. It is the level of a target that plays no volume LFO
// (PerformOptions::volume_lfos_as_mean_power).
struct VolumeSwing
{
    double shift = 0.0;
    double swing = 0.0;
    double power_mean = 0.0;
};
VolumeSwing VolumeSwingOf(const LfoShape& lfo, const PresetZone& zone);

// The swing of a volume LFO of `depth` steps of the note's level in dB, at the level of the zone's notes at a velocity
// of 127: half the difference between its two peaks around the zone's level, held from 1 to 127. It leaves out the
// LFO's offset and the shift of the zone's level. The converter takes VolumeSwingOf, and this stays for the API of
// 7.0.0.
double VolumeLfoDb(double depth, const PresetZone& zone);

// The sum of the shifts of the preset's volume LFOs in `shapes` for `zone`, in dB (VolumeSwing::shift).
double VolumeShiftDb(const std::vector<LfoShape>& shapes, const PresetZone& zone);

// The sum of the mean powers of the preset's volume LFOs in `shapes` for `zone`, in dB (VolumeSwing::power_mean).
double VolumeMeanDb(const std::vector<LfoShape>& shapes, const PresetZone& zone);

// The zone's root key and tuning in SoundFont's and SFZ's terms, for a sample of `sample_rate`: the voice's rate at
// the root key, without a bend, gives the tuning. The root key is the zone's key nearest to middle C. With `key`, the
// voice's rate at that key gives the tuning: the zone plays `key` at the timer's rate, within half a cent.
struct ZoneTuning
{
    bool operator==(const ZoneTuning&) const = default;

    int root_key = 60;
    int coarse = 0; // semitones
    int fine = 0;   // cents
};

ZoneTuning TuningOf(const PresetZone& zone, uint32_t sample_rate);
ZoneTuning TuningOf(const PresetZone& zone, uint32_t sample_rate, int key);

// A run of a zone's keys and the tuning that they share.
struct TunedZone
{
    PresetZone zone;
    ZoneTuning tuning;
};

// `zone`, in runs of its keys that take one tuning in whole cents (TuningOf at the key) for a sample of `sample_rate`,
// each run a zone of its own. The SoundFont's zones and the SFZ files' regions then play each key at the timer's rate,
// within half a cent. Where `keys` is given, only those keys take their tuning. Each other key joins the run of the
// nearest of those keys below it, or of the lowest above it. That adds no run. A zone without any of `keys` keeps the
// tuning at its root key.
std::vector<TunedZone> TunedZones(const PresetZone& zone, uint32_t sample_rate,
                                  const std::set<uint8_t>* keys = nullptr);

// The bend in semitones at which a SoundFont or SFZ player plays MIDI key `key` of `zone`, tuned as TunedZones tunes
// the key, at the rate of the voice's timer at `pitch`: the split's tuning, the key, the bend and the random pitch, in
// 256ths of a semitone (dse::VoiceTimer). The zone's tuning is in whole cents. Nothing where the timer gives no rate at
// `pitch` or at the key.
std::optional<double> TimerBend(const PresetZone& zone, uint32_t sample_rate, int key, uint16_t pitch);

// The sample rate of a wave of format `format` (byte 0x13) and rate `rate` (byte 0x20) in the SoundFont and the SFZ
// files: kPsgSampleRate for the PSG and the noise, and 32,728 Hz for a rate of 0. The zones' tunings give the pitch at
// any rate.
uint32_t SampleRateOf(uint8_t format, uint32_t rate);

// The banks of a song or a sound effect, with their decoded waves. A wave decodes once, on first use.
class BankSet
{
public:
    // The files must outlive the set.
    explicit BankSet(const SequenceFiles& files);

    // The bank of id `id` that the driver plays, the one loaded first, or null.
    const ntdlib::dse::WaveBank* Bank(uint16_t id) const;

    // The name of wave `wave` of bank `bank` among the sequence's samples: "w<bank>_<wave>".
    static std::string WaveName(uint16_t bank, uint16_t wave);

    // Gives the waves of each bank of `names` the name "<name>_<wave>" in the SFZ files (SfzBankNames).
    void SetSfzNames(std::map<uint16_t, std::string> names);

    // The name of wave `wave` of bank `bank` in the SFZ files: the one that SetSfzNames gives, or else WaveName's.
    std::string SfzWaveName(uint16_t bank, uint16_t wave) const;

    // The wave, or null for one that can't be read. A wave that can't be read gives a warning.
    const Pcm* Wave(uint16_t bank, uint16_t wave);

    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    std::map<uint16_t, const ntdlib::dse::WaveBank*> banks_;
    const ntdlib::dse::WaveBank* main_bank_ = nullptr;
    std::map<std::pair<uint16_t, uint16_t>, std::optional<Pcm>> waves_;
    std::map<uint16_t, std::string> sfz_names_;
    std::vector<std::string> warnings_;
};

} // namespace ntdlib::sequence_export::dse
