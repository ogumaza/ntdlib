// SPDX-License-Identifier: MIT

// The host's side of the Z2 start-up's parameters (drivers/jaudio/z2.h): where a driver's Z2Params hold the sound, its
// wave groups and the profile, and the words that a rip writes there. A driver's parameter block puts its header before
// them. The offsets count from the block's Z2Params.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/flipper/wave_list.h"
#include "ntdlib/jaudio/z2_states.h"

namespace ntdlib::flipper::z2
{

// Where Z2Params hold the sound's JAudio ID, its JAISoundHandle, the number of wave groups, the groups and the profile.
// The wave list (wave_list.h) starts at kWaveCountOffset. The piece's state follows the profile.
inline constexpr uint32_t kSoundOffset = 0x00;
inline constexpr uint32_t kHandleOffset = 0x04;
inline constexpr uint32_t kWaveCountOffset = 0x08;
inline constexpr uint32_t kWavesOffset = 0x0c;
inline constexpr uint32_t kProfileOffset = kWavesOffset + 4 * kMaxWaves;

// The most first-scene groups that a profile lists.
inline constexpr uint32_t kMaxFirstSceneWaves = 4;

// The most other start-up groups that a profile lists for room in the wave heap.
inline constexpr uint32_t kMaxRoomWaves = 4;

// The profile: the sizes that the game's start-up gives and the addresses of the game's functions and variables, from
// frame_retraces on. drivers/jaudio/z2.h describes each. A console leaves the fields of steps that it doesn't take at
// 0, such as the video interface on the Wii. The first scene's groups follow. The driver erases those that a sound
// doesn't use before it loads the sound's groups. More of the start-up's groups come last. The driver erases them only
// when the wave heap has no room for one of the sound's groups. A console that erases none leaves both lists at 0.
struct Profile
{
    uint32_t frame_retraces = 0;
    uint32_t max_heaps = 0;
    uint32_t system_heap_size = 0;
    uint32_t aram_audio_size = 0;
    uint32_t aram_graph_size = 0;
    uint32_t aram_priorities = 0;
    uint32_t aram_buffer_size = 0;
    uint32_t command_heap_size = 0;
    uint32_t archive_heap_size = 0;
    uint32_t zelda_heap_margin = 0;
    uint32_t audio_heap_size = 0;
    uint32_t vi_init = 0;
    uint32_t vi_wait_for_retrace = 0;
    uint32_t get_current_thread = 0;
    uint32_t get_thread_priority = 0;
    uint32_t create_root_heap = 0;
    uint32_t create_exp_heap = 0;
    uint32_t create_solid_heap = 0;
    uint32_t create_aram = 0;
    uint32_t set_aram_buffer = 0;
    uint32_t create_thread_switch = 0;
    uint32_t create_command_heap = 0;
    uint32_t create_archive_heap = 0;
    uint32_t archive_heap_parent = 0;
    uint32_t create_assert_heap = 0;
    uint32_t create_zelda_heap = 0;
    uint32_t get_free_size = 0;
    uint32_t become_current_heap = 0;
    uint32_t system_heap = 0;
    uint32_t alloc = 0;
    uint32_t thread_ctor = 0;
    uint32_t create_disc_thread = 0;
    uint32_t audio_heap = 0;
    uint32_t audio_setup = 0;
    uint32_t init_flag = 0;
    uint32_t audio_frame = 0;
    uint32_t load_se_wave = 0;
    uint32_t load_bgm_wave = 0;
    uint32_t wave_status = 0;
    uint32_t audio_manager = 0;
    uint32_t start_sound = 0;
    uint32_t erase_bgm_wave = 0;
    uint32_t erase_se_wave = 0;
    uint32_t first_scene_wave_count = 0;
    std::array<uint32_t, kMaxFirstSceneWaves> first_scene_waves{}; // (bank << 16) | group, as in WaveList
    uint32_t room_wave_count = 0;
    std::array<uint32_t, kMaxRoomWaves> room_waves{}; // (bank << 16) | group, as in WaveList
    // The variable that holds the music manager, Z2SeqMgr, and its setChildTrackVolume. After the driver starts a
    // sound, it gives the children of track 0 the volumes that a rip writes from kMutedChildrenOffset on. A console
    // that sets none leaves both at 0.
    uint32_t music_manager = 0;
    uint32_t set_child_volume = 0;
    // Z2SoundStarter::setPortData. The driver writes a rip's status (kMusicStatusOffset) to port 9 of the sound's root
    // track with it. A console that writes none leaves it at 0.
    uint32_t set_port_data = 0;
    // Z2SoundStarter::getPortData. The driver reads port 10 of the sound's root track with it, for a rip's statuses by
    // section (kSectionStatusesOffset). A console that writes none leaves it at 0.
    uint32_t get_port_data = 0;

    // The words in Z2Params' order, for kProfileOffset on.
    std::vector<uint32_t> Words() const;
};

// Where Z2Params hold the state that a rip gives its piece of music (PieceState), and the size of Z2Params. A
// console's driver may add fields after kParamsSize.
inline constexpr uint32_t kMutedChildrenOffset = kProfileOffset + sizeof(Profile);
inline constexpr uint32_t kMaxPartialChildren = 16;
inline constexpr uint32_t kPartialChildrenOffset = kMutedChildrenOffset + 4;
inline constexpr uint32_t kPartialVolumesOffset = kPartialChildrenOffset + 4;
inline constexpr uint32_t kMusicStatusOffset = kPartialVolumesOffset + 4 * kMaxPartialChildren;
inline constexpr uint32_t kSectionStatusesOffset = kMusicStatusOffset + 4;
inline constexpr uint32_t kParamsSize = kSectionStatusesOffset + 4;

// The port of the sound's root track that holds a piece's status, the flag of a status that the driver writes, and the
// flag of a status that it writes 30 frames after the start (MusicState::status_later).
inline constexpr uint32_t kMusicStatusPort = 9;
inline constexpr uint32_t kMusicStatusSet = 0x10000;
inline constexpr uint32_t kMusicStatusLater = 0x20000;

// The state that a rip gives its piece of music, from kMutedChildrenOffset on: the children of the sound's track 0 that
// start at a volume of 0, bit n for child n; the children at a volume between 0 and 1, bit n for child n, with their
// volumes by child; kMusicStatusSet | the status that the driver writes to port 9, with kMusicStatusLater for one
// that it writes 30 frames after the start, or 0 for none; and the statuses that
// the driver writes in each frame by the section on port 10. Each half of that word holds (section << 8) | status.
struct PieceState
{
    uint32_t muted_children = 0;
    uint32_t partial_children = 0;
    std::array<float, kMaxPartialChildren> partial_volumes{};
    uint32_t music_status = 0;
    uint32_t section_statuses = 0;

    // The state's words, big-endian, from kMutedChildrenOffset up to the last word that isn't 0. A piece without a
    // state has none. A rip writes them in its mini file.
    std::vector<uint8_t> Bytes() const;

    bool operator==(const PieceState&) const = default;
};

// The state of a piece that a table of states gives (jaudio/z2_states.h): a child at a volume of 0 is muted, a child at
// another volume below 1 is partial, and the state's statuses go to port 9.
PieceState PieceStateOf(const jaudio::MusicState& state);

} // namespace ntdlib::flipper::z2
