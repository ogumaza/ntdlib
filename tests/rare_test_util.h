// SPDX-License-Identifier: MIT

// What the tests of Rare's driver share: the names they use and the detection of a test cartridge.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/song_plan.h"
#include "ntdlib/rare/archive_player.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/rare/sequencer.h"
#include "ntdlib/rare/song.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/rare/convert.h"
#include "ntdlib/sequence_export/rare/performance.h"
#include "ntdlib/sequence_export/rare/soundfont.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/rare_cart.h"

namespace ntdlib::sequence_export::rare
{

using ntdlib::agb::kFrameRate;
using ntdlib::agb::kRomBase;
using ntdlib::agb::PathFromUtf8;
using ntdlib::agb::Rom;
using ntdlib::agb::Utf8;
using ntdlib::rare::Action;
using ntdlib::rare::ArchivePlayer;
using ntdlib::rare::DecodeEvent;
using ntdlib::rare::DescribeEvent;
using ntdlib::rare::DetectDriver;
using ntdlib::rare::DriverInfo;
using ntdlib::rare::DriverOverrides;
using ntdlib::rare::EnvelopeSettings;
using ntdlib::rare::Event;
using ntdlib::rare::FadeFrames;
using ntdlib::rare::Format;
using ntdlib::rare::Instrument;
using ntdlib::rare::kCmdBend;
using ntdlib::rare::kCmdControl;
using ntdlib::rare::kCmdDelay1;
using ntdlib::rare::kCmdDelay2;
using ntdlib::rare::kCmdDelay3;
using ntdlib::rare::kCmdEnd;
using ntdlib::rare::kCmdNoteOff;
using ntdlib::rare::kCmdNoteOn;
using ntdlib::rare::kCmdNoteOnB;
using ntdlib::rare::kCmdProgram;
using ntdlib::rare::kCmdTempo;
using ntdlib::rare::kCtrlAttack;
using ntdlib::rare::kCtrlDecay;
using ntdlib::rare::kCtrlLoopEnd;
using ntdlib::rare::kCtrlLoopStart;
using ntdlib::rare::kCtrlModulation;
using ntdlib::rare::kCtrlRelease;
using ntdlib::rare::kCtrlSustain;
using ntdlib::rare::kCtrlVolume;
using ntdlib::rare::kFullLevel;
using ntdlib::rare::kInstDrumKit;
using ntdlib::rare::kInstKeySplit;
using ntdlib::rare::kInstSample;
using ntdlib::rare::kLoopForward;
using ntdlib::rare::kLoopForwardB;
using ntdlib::rare::kLoopOnce;
using ntdlib::rare::kSlotFree;
using ntdlib::rare::kSlotOn;
using ntdlib::rare::kSlotReleased;
using ntdlib::rare::kTempoScale;
using ntdlib::rare::LookUpInstrument;
using ntdlib::rare::MixResult;
using ntdlib::rare::MusicVolume;
using ntdlib::rare::ReadInstrument;
using ntdlib::rare::ReadTuneHeader;
using ntdlib::rare::Sequencer;
using ntdlib::rare::SilencedChannels;
using ntdlib::rare::Slot;
using ntdlib::rare::SongCount;
using ntdlib::rare::TuneAddress;
using ntdlib::rare::TuneHeader;
using ntdlib::sequence_export::agb::kDrumBank;
using ntdlib::sequence_export::agb::PresetInstrument;
using ntdlib::sequence_export::agb::VolumeEnvelope;

using ntdlib::testing::agb_files::HasGlobalZoneModulators;
using ntdlib::testing::agb_files::Le16;
using ntdlib::testing::agb_files::Le32;
using ntdlib::testing::agb_files::MidiEvents;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::agb_files::Sf2Records;
using ntdlib::testing::agb_files::TempPath;
using ntdlib::testing::agb_files::TempSettings;
using ntdlib::testing::rare_cart::Cart;
using ntdlib::testing::rare_cart::kInit;
using ntdlib::testing::rare_cart::kPitchTable;
using ntdlib::testing::rare_cart::kSineTable;
using ntdlib::testing::rare_cart::kTuneTable;
using ntdlib::testing::rare_cart::Saw;
using ntdlib::testing::rare_cart::Track;

// Returns the driver info of a cartridge, failing the test if detection fails.
inline DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));

    return info;
}

} // namespace ntdlib::sequence_export::rare
