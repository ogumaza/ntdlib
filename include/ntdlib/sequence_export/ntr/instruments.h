// SPDX-License-Identifier: MIT

// The instruments of a DS bank (SBNK) as the zones of SoundFont and SFZ presets, the waves that their regions play, and
// the envelopes of the DS's sound driver in SoundFont's and SFZ's terms.
//
// A region plays a wave of a wave archive (PCM8, PCM16 or IMA-ADPCM), a rectangular wave of the PSG at one of its
// eight duties, or the noise generator. A channel of the sound unit plays a PCM wave at its timer's rate, without
// interpolation. The PSG plays its eight steps, and the noise generator its steps of a 15-bit shift register, at the
// rate of a timer of 8006 at the region's root key: 2093 steps a second. Middle C then plays at the root key. The
// samples here hold each step of the PSG and of the noise 16 times over, at 16 times that rate. The SoundFont and SFZ
// players' interpolation then rounds off little of their edges.
//
// The driver works out a note's timer from the wave's timer and the note's pitch, and drops the fraction of the
// period. A PSG channel also loses the period's low two bits. A key therefore plays up to 1200 * log2(P / (P - 1))
// cents sharp of its nominal pitch, for a period of P. A zone's tuning gives each key the channel's rate (TunedZones).
//
// The driver's envelope steps once a frame (about 5.21 ms). Its attack multiplies the attenuation by a factor each
// frame, from 72.3 dB down. Its decay and release fall at a rate in dB a frame. A sustain or a volume of v plays at the
// level of (v/127)^2.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "seqlib/pcm.h"
#include "seqlib/sf2/units.h"

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/sequence_export/psg_samples.h"

namespace ntdlib::sequence_export::ntr
{

// A decoded wave, the rate of its samples in the files (SampleRate), and the channel that plays it: its type
// (snd::Channel::type: 0 PCM, 1 PSG, 2 noise) and its timer at the root key. A wave whose timer is 0 plays at its
// sample rate.
struct Pcm : seqlib::Pcm
{
    uint32_t sample_rate = 0;
    uint8_t channel_type = 0;
    uint16_t timer = 0;
};

// The rate of the samples of the PSG's and the noise's waves (psg_samples.h).
using ntdlib::sequence_export::kPsgSampleRate;

// The rate of the samples in the files of a wave that a channel of `channel_type` plays with the timer `timer` at its
// root key: 16,756,991 / timer, rounded, or kPsgSampleRate for the PSG and the noise.
uint32_t SampleRate(uint8_t channel_type, uint16_t timer);

// The cents above SampleRate at which a channel of `channel_type` plays its wave at `pitch` 64ths of a semitone from
// the root key, with the timer `timer` at the root key (snd::Timer). The nominal pitch for a timer of 0.
double ChannelCents(uint8_t channel_type, uint16_t timer, int pitch);

// The tuning in cents that a zone of a region with root key `root_key` takes at key `key`: the channel's rate there
// (ChannelCents) against the pitch at which a SoundFont or SFZ player plays the key. The players' root key is at most
// 127.
int KeyTuning(uint8_t channel_type, uint16_t timer, int root_key, int key);

// The bend in semitones at which a player plays a note of key `key`, in a zone of KeyTuning's tuning, at the channel's
// rate for `pitch` 64ths of a semitone from the root key.
double TimerBend(uint8_t channel_type, uint16_t timer, int root_key, int key, int pitch);

// The most, in semitones, by which TimerBend can differ from the bend itself for a note of key `key` at `pitch`: the
// rounding of the period at `pitch` or of the period at the key that sets the zone's tuning, whichever is larger, and
// half a cent for the tuning's whole cents. A period P loses up to one count, 1200 log2((P + 1) / P) cents, and the
// PSG's period up to four. 0 for a timer of 0.
double TimerRounding(uint8_t channel_type, uint16_t timer, int root_key, int key, int pitch);

// A note's envelope in the driver's values, from 0 to 127.
struct Envelope
{
    uint8_t attack = 127;
    uint8_t decay = 127;
    uint8_t sustain = 127;
    uint8_t release = 127;
};

// A zone of a preset: a region of the program's instrument, the keys that play it, its envelope with the track's
// values, and its tuning in cents (KeyTuning, TunedZones).
struct PresetZone
{
    uint8_t low_key = 0;
    uint8_t high_key = 127;
    sdat::Region region;
    Envelope envelope;
    int tuning = 0;
};

// The zones of `key`'s program in `bank`. A drum set gives each of its keys a zone, and a key split each of its ranges
// of keys. A region that plays nothing has no zone: one of no type, a dummy, or one that plays the wave at an address
// that the game's code sets (InstrumentType::kDirectPcm).
std::vector<PresetZone> PresetZones(const sdat::Bank& bank, const PresetKey& key);

// `zone`, in runs of its keys that take one tuning (KeyTuning) when a channel plays `wave`, each run a zone of its own.
// The SoundFont's zones and the SFZ files' regions then play each key at the channel's rate, within half a cent. Where
// `keys` is given, only those keys take their tuning. Each other key joins the run of the nearest of those keys below
// it, or of the lowest above it. That adds no run. A zone without any of `keys` keeps the region's tuning of 0.
std::vector<PresetZone> TunedZones(const PresetZone& zone, const Pcm& wave, const std::set<uint8_t>* keys = nullptr);

// The SoundFont's volume envelope: times in timecents, and the sustain in centibels of attenuation.
struct Sf2Envelope
{
    int attack = seqlib::sf2::kShortestTimecents, hold = seqlib::sf2::kShortestTimecents,
        decay = seqlib::sf2::kShortestTimecents, sustain = 0, release = seqlib::sf2::kShortestTimecents;
};

// The envelope in SoundFont's terms. SoundFont's attack rises linearly in amplitude. Its time comes from a fit to the
// driver's attack, and the hold takes the rest of the driver's attack. As in the driver, the decay and the release fall
// linearly in dB. Their times give a fall of 100 dB.
Sf2Envelope ConvertEnvelope(const Envelope& e, const ntdlib::ntr::snd::Build& build);

// SFZ's amplitude envelope: times in seconds, and the sustain in percent of full amplitude.
struct SfzEnvelope
{
    double attack = 0.0, hold = 0.0, decay = 0.0, sustain = 100.0, release = 0.0;
};

// The envelope in SFZ's terms, with SoundFont's attack and hold. The decay and the release fall by a factor of e^-8
// (69.49 dB) over their time. sfzformat.com's ampeg_attack page gives that fall.
SfzEnvelope ConvertEnvelopeForSfz(const Envelope& e, const ntdlib::ntr::snd::Build& build);

// The bank of a sequence, with its wave archives and their decoded waves. A wave decodes once, on first use.
class BankSet
{
public:
    // The archive must outlive the set. A bank that can't be read gives warnings, and no zones.
    BankSet(const sdat::Archive& archive, uint16_t bank);

    // The bank, or null when the archive has none.
    const sdat::Bank* Bank() const
    {
        return bank_ ? &*bank_ : nullptr;
    }

    // The name of the wave that `region` plays among an archive's samples: "w<wave archive>_<wave>" by the number of
    // the archive's wave archive, "psg<duty>" or "noise".
    std::string WaveName(const sdat::Region& region) const;

    // The wave that `region` plays, or null for a wave that can't be read. A wave that can't be read gives a warning.
    const Pcm* Wave(const sdat::Region& region);

    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    // The archive's number of the wave archive in the bank's slot `slot`, or nothing.
    std::optional<uint16_t> WaveArchiveOf(uint16_t slot) const;

    const sdat::Archive& archive_;
    uint16_t number_ = 0;
    std::optional<sdat::Bank> bank_;
    std::array<uint16_t, sdat::kWaveArchiveSlots> wave_archives_{sdat::kNoWaveArchive, sdat::kNoWaveArchive,
                                                                 sdat::kNoWaveArchive, sdat::kNoWaveArchive};
    std::map<uint16_t, std::optional<sdat::WaveArchive>> parsed_;
    std::map<std::string, std::optional<Pcm>> waves_;
    std::vector<std::string> warnings_;
};

} // namespace ntdlib::sequence_export::ntr
