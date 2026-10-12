// SPDX-License-Identifier: MIT

// The settings of each console, for sequence_export's performer and conversion: one function for each console. Programs
// and ntdlib's tests take them from here. Settings' defaults are the Wii U's values. Each function sets the fields in
// which its console differs.

#pragma once

#include <cstdint>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sound_build.h"

namespace ntdlib::sequence_export
{

// The Wii U, with nw::snd and AX. The archive reader reads a group's whole file table before it locates any of the
// group's files. A note that finds none of nw::snd's 96 voices free is dropped (voice_limit). The count is the .data
// word at 0x101AA478. A game can change it before its sound system starts: Animal Crossing: amiibo Festival sets 78.
// The core's channels follow nw::snd's ChannelManager::Alloc (0x022B8A14). Each channel has a voice: once every channel
// is in use, a new note finds none and is dropped.
inline Settings CafeSettings()
{
    Settings settings;
    settings.engine = Engine::kCafe;
    settings.archive = {sar::Flavor::kCafe, {}, true};
    settings.filters = &kCafeFilters;
    settings.voice_limit = 96;

    return settings;
}

// The Switch, with nn::atk and its audio renderer. Its engine runs a 5 ms frame, steps its random generator once before
// a sound starts, reads the bytecode's arguments in the file's byte order, releases a note whose length ends in its
// first frame at the next update, and sends the main bus and buses B and C to the mix dry. The Switch has no
// voice_limit: the core doesn't model nn::atk's drops yet.
inline Settings SwitchSettings()
{
    Settings settings;
    settings.engine = Engine::kSwitch;
    settings.archive = {sar::Flavor::kNx, {}, true};
    settings.filters = &kSwitchFilters;
    settings.frame_ms = 5.0;
    settings.env_step_ms = 5;
    settings.random_steps_before_start = 1;
    settings.arguments_follow_byte_order = true;
    settings.note_off_waits_for_update = true;
    settings.sends = Sends::kReverbDry;

    return settings;
}

// The 3DS, with nw::snd on the DSP. Its frame is 160 samples at 268111856 / 8192 Hz, and its envelopes step 5 ms at a
// time. A track starts at most 24 voices a tick. The random generator steps 17 times before a sound starts. The effect
// sends are left out. A legato leaves out its first region's volume, and no tick comes early. The archive reader
// locates a group's files as it reads the group's table. A note that finds every one of nw::snd's 24 voices in use
// takes the voice of the lowest priority (VoiceManager::AllocVoice, 0x187070). If every voice outranks it, the note is
// dropped (voice_limit). A game can change the count: Pokemon X sets 23 (code.bin 0x1150d0). The core leaves out
// nn::snd's drops of DSP voices. A note that the game drops there plays on. Such a note can cost a later note its
// voice.
inline Settings CtrSettings()
{
    Settings settings;
    settings.engine = Engine::kCtr;
    settings.archive = {sar::Flavor::kCtr, {}, false};
    settings.filters = &kCtrFilters;
    settings.frame_ms = 160.0 * 8192.0 * 1000.0 / 268111856.0;
    settings.env_step_ms = 5;
    settings.env_time_scale = settings.frame_ms / settings.env_step_ms;
    settings.voices = 24;
    settings.voice_limit = 24;
    settings.random_steps_before_start = 17;
    settings.sends = Sends::kNone;
    settings.keep_first_region_volume = false;
    settings.early_ticks_ms = 0.0;

    return settings;
}

// The Wii, with NW4R SND and AX, for the build of NW4R SND that plays an archive. The builds of October 2006 start at
// most 64 voices a tick. The Wii's archives are read with ntdlib's rsar. `archive` therefore keeps its defaults.
inline Settings RevolutionSettings(const SndBuild& build)
{
    Settings settings;
    settings.engine = Engine::kRevolution;
    settings.voices = build.October2006() ? 64 : 96;
    settings.sends = Sends::kReverbChorus;
    settings.surround_pan = true;
    settings.keep_first_region_volume = false;
    settings.filters = &kRevolutionFilters;
    settings.wii_build = build;
    settings.early_ticks_ms = 0.0;
    settings.subroutine_loop_rule = false;
    settings.stop_runaway_tracks = false;
    settings.drop_loop_when_finished = false;

    return settings;
}

// The build of NW4R SND that plays a Wii archive of RSAR version `version` when the game's main.dol isn't at hand: a
// build from June 2008 for version 1.4 or later, and otherwise a build of 2007.
inline SndBuild BuildOfRsarVersion(uint16_t version)
{
    SndBuild build;
    build.date = version >= 0x0104 ? 20080620 : 0;

    return build;
}

} // namespace ntdlib::sequence_export
