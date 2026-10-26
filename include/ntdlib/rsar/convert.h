// SPDX-License-Identifier: MIT

// The Wii's banks, waves and streams as files of the 3DS, the Wii U or the Switch, and the later consoles' banks and
// waves as the Wii's. A bank of NW4R SND (RBNK) holds each instrument as a tree of key and velocity tables that lead to
// regions. The later consoles' banks hold key regions and velocity regions instead (sar/bank_file.h). A converted bank
// plays the region that NW4R SND finds for each key and velocity (Bank::Find), with the values that NW4R SND's build
// reads. A bank for the Wii plays the later bank's regions at the same keys and velocities. A converted wave holds the
// samples in their encoding. A converted stream holds the Wii's blocks.
//
// The later consoles' streams, wave archives and wave sound files go to the Wii too (WiiStreamOf, WiiWaveArchiveOf and
// WiiWsdOf). A stream for the Wii holds the later stream's blocks.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"

namespace ntdlib::rsar
{

// `wave`, a wave of the Wii, as a wave file of `flavor` and `version`: its encoding, its rate, its loop, and the
// samples of its first two channels up to the wave's end. NW4R SND plays only those. A DSP-ADPCM channel keeps its
// coefficients, its history at the start and its state at the loop's start. A 16-bit sample takes the file's byte
// order. The loop's original start is the loop's start. The state at the loop's start comes with the readers' playback
// fields. Without them, it reads as zeros. Throws std::invalid_argument for a wave without channels, and for a channel
// whose samples end before the wave's end.
sar::WaveFile WaveFileOf(const Wave& wave, sar::Flavor flavor, uint32_t version);

// `bank`, a bank of the Wii that `build` of NW4R SND plays, read with its waves, as a bank file of `flavor` and
// `version`. Wave i of the bank's waves becomes entry i of the table of waves: wave i of the wave archive with item ID
// `wave_archive`. Each program's instrument holds a key region for each run of keys that play the same regions at each
// velocity. Each key region holds a velocity region for each run of velocities that play the same region. A region
// that names no wave of the bank plays nothing, as on the Wii. A program without a region at any key and velocity has
// no instrument. A velocity region takes the region's wave, original key, volume and pan, its tune as the pitch, and
// its envelope. Builds before March 2008 read no hold, note-off type or key group, and the region then takes none.
// NW4R SND's voices interpolate with the 4-tap filters, and so do the regions. NW4R SND's stereo mix reads no surround
// pan, and the bank leaves it out.
sar::BankFile BankFileOf(const Bank& bank, const SndBuild& build, sar::Flavor flavor, uint32_t version,
                         uint32_t wave_archive);

// `stream`, a stream of the Wii, as a stream file of `flavor` and `version`: its encoding, its rate, its loop, its
// tracks and every channel. The later consoles lay their blocks out as the Wii does. Each channel's blocks therefore
// keep their bytes, and 16-bit samples take the file's byte order. A DSP-ADPCM channel keeps its info. SEEK takes
// ADPC's entry for each block's start, or the decoded samples before the block where ADPC holds no entry there. A
// stream in PCM holds no ADPC, and SEEK takes the samples before each block. A track keeps its channels, its volume and
// its pan, with a span and flags of 0. The loop's original start and end are the loop's. sar::StreamFile::Write lays
// the file out in `version`. The later consoles' tools start a loop at a block. Throws std::invalid_argument for a loop
// that starts inside a block.
sar::StreamFile StreamFileOf(const StreamFile& stream, sar::Flavor flavor, uint32_t version);

// `stream`, a stream file of the 3DS, the Wii U or the Switch, as a stream of the Wii: its encoding, its rate, its
// loop, its tracks and every channel. The Wii lays its blocks out as the later consoles do. Each channel's blocks
// therefore keep their bytes, without the samples past the end that the 3DS's tools encode, and 16-bit samples take the
// Wii's byte order. A DSP-ADPCM channel keeps its info, with a gain of 0. ADPC takes SEEK's entry for each block's
// start, a block's samples apart. A track keeps its channels, its volume and its pan in an extended info. A stream
// without tracks takes a track for each pair of its channels, the last one alone, at a volume of 127 and the centre.
// NW4R SND loops a stream from the start of the block that holds the loop's start. Throws std::invalid_argument for an
// encoding that the Wii doesn't play, such as IMA-ADPCM, a loop that starts inside a block, more than 255 channels or
// tracks, and a rate past 24 bits.
StreamFile WiiStreamOf(const sar::StreamFile& stream);

// `wave`, a wave file of the 3DS, the Wii U or the Switch, as a wave of the Wii: its encoding, its rate, its loop and
// the samples of its first two channels up to the wave's end. NW4R SND plays only those. A DSP-ADPCM channel keeps its
// coefficients, its history at the start and its state at the loop's start. A 16-bit sample takes the Wii's byte order.
// The loop starts where `wave` starts it, at its loop's start rather than its original start. The wave takes RWAV's
// version 1.2. Throws std::invalid_argument for a wave without channels, for a channel whose samples end before the
// wave's end, and for a rate past 24 bits.
WaveFile WiiWaveOf(const sar::WaveFile& wave);

// `bank`, a bank file of the 3DS, the Wii U or the Switch, as a bank of the Wii of `version` with `waves`. Entry i of
// `waves` gives the wave of entry i of the bank's table of waves. An entry without a wave stands for a wave that the
// archive lacks, such as an entry of the 3DS that names wave 0xFFFFFFFF. The bank leaves it out, and its regions take
// the wave index -1: NW4R SND plays nothing there. Each instrument keeps its key regions and velocity regions, laid out
// as the tools lay a bank out (BankFile::FromInstruments). A region takes the velocity region's wave, original key,
// volume and pan, its pitch as the tune, its envelope, its note-off type and its key group. NW4R SND's voices
// interpolate with the 4-tap filters, and the bank holds no interpolation. Throws std::invalid_argument for a region
// whose wave lies past `waves`, and the exceptions of BankFile::FromInstruments.
BankFile WiiBankOf(const sar::BankFile& bank, uint16_t version, const std::vector<std::optional<WaveFile>>& waves);

// `archive`, a wave archive file of the 3DS, the Wii U or the Switch, as a wave archive of the Wii (RWAR): each of its
// waves as WiiWaveOf makes it, in the archive's order. Throws xsflib::FormatError for a wave that the reader can't
// read, such as a wave in IMA-ADPCM, and the exceptions of WiiWaveOf and WaveFile::Write.
WaveArchiveFile WiiWaveArchiveOf(const sar::WaveArchiveFile& archive);

// `file`, a wave sound file of the 3DS, the Wii U or the Switch as the reader reads it, as a wave sound file of the Wii
// of `version` with `waves`. Entry i of `waves` gives the wave of entry i of the file's table of waves, as for
// WiiBankOf. An entry without a wave stands for a wave that the archive lacks. The file leaves it out, and the notes
// that name it take the wave index -1: NW4R SND plays nothing there. Each wave sound keeps its pitch, its pan, its
// surround pan, its sends and its tracks of note events. The later consoles' libraries play a wave sound's first note
// with the wave sound's envelope, and NW4R SND with the note's. The first note therefore takes the wave sound's
// envelope. Each other note keeps its envelope. Every note keeps its key, its volume, its pan and its pitch, which
// neither console reads. The Wii's wave sounds hold no filters. Builds of NW4R SND before March 2008 read no hold.
// Throws std::invalid_argument for a note whose wave lies past `waves`.
WsdFile WiiWsdOf(const sar::WaveSoundFile& file, uint16_t version, const std::vector<std::optional<WaveFile>>& waves);

} // namespace ntdlib::rsar
