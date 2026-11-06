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
// The driver's envelope steps once a frame (about 5.21 ms). Its attack multiplies the attenuation by a factor each
// frame, from 72.3 dB down. Its decay and release fall at a rate in dB a frame. A sustain or a volume of v plays at the
// level of (v/127)^2.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "seqlib/pcm.h"

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/sequence_export/psg_samples.h"

namespace ntdlib::sequence_export::ntr
{

// A decoded wave, and the rate at which the DS's channel plays it at its root key.
struct Pcm : seqlib::Pcm
{
    uint32_t sample_rate = 0;
};

// The rate of the samples of the PSG's and the noise's waves (psg_samples.h).
using ntdlib::sequence_export::kPsgSampleRate;

// A note's envelope in the driver's values, from 0 to 127.
struct Envelope
{
    uint8_t attack = 127;
    uint8_t decay = 127;
    uint8_t sustain = 127;
    uint8_t release = 127;
};

// A zone of a preset: a region of the program's instrument, the keys that play it, and its envelope with the track's
// values.
struct PresetZone
{
    uint8_t low_key = 0;
    uint8_t high_key = 127;
    sdat::Region region;
    Envelope envelope;
};

// The zones of `key`'s program in `bank`. A drum set gives each of its keys a zone, and a key split each of its ranges
// of keys. A region that plays nothing has no zone: one of no type, a dummy, or one that plays the wave at an address
// that the game's code sets (InstrumentType::kDirectPcm).
std::vector<PresetZone> PresetZones(const sdat::Bank& bank, const PresetKey& key);

// The SoundFont's volume envelope: times in timecents, and the sustain in centibels of attenuation.
struct Sf2Envelope
{
    int attack = -12000, hold = -12000, decay = -12000, sustain = 0, release = -12000;
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

    // The bank, or null when the archive lacks it.
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
    std::array<uint16_t, 4> wave_archives_{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    std::map<uint16_t, std::optional<sdat::WaveArchive>> parsed_;
    std::map<std::string, std::optional<Pcm>> waves_;
    std::vector<std::string> warnings_;
};

} // namespace ntdlib::sequence_export::ntr
