// SPDX-License-Identifier: MIT

// The programs of DSE's wave banks as the zones of SoundFont and SFZ presets, the waves that their splits play, and
// DSE's envelopes and LFOs in SoundFont's and SFZ's terms.
//
// A split of a program plays a wave of its bank for the keys and velocities in its ranges. A note takes a voice for
// each split that holds its key and velocity, and so does a SoundFont or SFZ note for each zone or region. A voice
// plays its wave at the rate of its pitch: the driver's table gives each pitch a rate, and the split's tuning moves the
// key's pitch. A bank's waves can lie in the main bank's samples. The sound unit plays the waves without interpolation.
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
#include <string>
#include <utility>
#include <vector>

#include "seqlib/pcm.h"

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

// The zones of `key`'s program in `bank`, one for each split in the order of the driver's search. A split whose wave
// the bank lacks has no zone.
std::vector<PresetZone> PresetZones(const ntdlib::dse::WaveBank& bank, const PresetKey& key);

// The volume envelope in SoundFont's terms: times in timecents, and the sustain in centibels of attenuation. The decay
// and the release times give a fall of 100 dB.
struct Sf2Envelope
{
    int attack = -12000, hold = -12000, decay = -12000, sustain = 0, release = -12000;
};

Sf2Envelope ConvertEnvelope(const ntdlib::dse::EnvelopeValues& e);

// The amplitude envelope in SFZ's terms: times in seconds, and the sustain in percent of full amplitude. The decay and
// the release fall by a factor of e^-8 (69.49 dB) over their time. sfzformat.com's ampeg_attack page gives that fall.
struct SfzEnvelope
{
    double attack = 0.0, hold = 0.0, decay = 0.0, sustain = 100.0, release = 0.0;
};

SfzEnvelope ConvertEnvelopeForSfz(const ntdlib::dse::EnvelopeValues& e);

// An LFO of a preset in the units that SoundFont and SFZ take: its rate in Hz, its delay and fade in seconds, and half
// its swing: in cents of pitch, in steps of the note's level from 0 to 127, or in steps of the pan from 0 to 127.
// `triangle` is set when the driver's waveform is the triangle that swings from 0 up to the top first and around 0,
// as SoundFont's LFOs do.
struct LfoShape
{
    uint8_t output = 0; // 1 the pitch, 2 the volume and 3 the pan
    uint8_t waveform = 0;
    double hz = 0.0;
    double delay = 0.0;
    double fade = 0.0;
    double depth = 0.0;
    bool triangle = false;
};

// The preset's LFOs that change the sound, in the order of their slots.
std::vector<LfoShape> PresetLfos(const PresetKey& key);

// The swing of a volume LFO of `depth` steps of the note's level in dB, at the level of the zone's notes at a velocity
// of 127: half the difference between its two peaks. The driver adds the LFO to the level before it squares it.
double VolumeLfoDb(double depth, const PresetZone& zone);

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
    std::vector<std::string> warnings_;
};

} // namespace ntdlib::sequence_export::dse
