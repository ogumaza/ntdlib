// SPDX-License-Identifier: MIT

// The conversion of JAudio's sound data to MIDI, SoundFont 2 and SFZ. The data comes from the jaudio module's readers:
// JAudio2's start-up archive (BAA) with its sound table, sequence collection, wave systems (WSYS) and instrument banks
// (IBNK), and the sequence archive (an RARC of BMS files) beside it.

#pragma once

#include "xsflib/binary.h"

#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"

namespace ntdlib::sequence_export::jaudio
{

// Invalid or unsupported input.
using xsflib::FormatError;

using ntdlib::jaudio::ArchiveFile;
using ntdlib::jaudio::Bank;
using ntdlib::jaudio::DecodeYaz0;
using ntdlib::jaudio::Drum;
using ntdlib::jaudio::DrumSet;
using ntdlib::jaudio::InstEffect;
using ntdlib::jaudio::Instrument;
using ntdlib::jaudio::IsYaz0;
using ntdlib::jaudio::KeyRegion;
using ntdlib::jaudio::LayoutOf;
using ntdlib::jaudio::Oscillator;
using ntdlib::jaudio::OscPoint;
using ntdlib::jaudio::ReadRarc;
using ntdlib::jaudio::SoundArchive;
using ntdlib::jaudio::SoundEntry;
using ntdlib::jaudio::VelocityRegion;
using ntdlib::jaudio::WaveFormat;
using ntdlib::jaudio::WaveInfo;
using ntdlib::jaudio::WaveSystem;

} // namespace ntdlib::sequence_export::jaudio
