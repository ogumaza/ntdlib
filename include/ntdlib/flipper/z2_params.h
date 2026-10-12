// SPDX-License-Identifier: MIT

// The host's side of the Z2 start-up's parameters (drivers/jaudio/z2.h): where a driver's Z2Params hold the sound, its
// wave groups and the profile, and the words that a rip writes there. A driver's parameter block puts its header before
// them. The offsets count from the block's Z2Params.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::flipper::z2
{

// Where Z2Params hold the sound's JAudio ID, its JAISoundHandle, the number of wave groups, the groups and the profile.
inline constexpr uint32_t kSoundOffset = 0x00;
inline constexpr uint32_t kHandleOffset = 0x04;
inline constexpr uint32_t kWaveCountOffset = 0x08;
inline constexpr uint32_t kWavesOffset = 0x0c;
inline constexpr uint32_t kMaxWaves = 64;
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

    // The words in Z2Params' order, for kProfileOffset on.
    std::vector<uint32_t> Words() const;
};

// The words that a mini file or a length analysis writes at kWaveCountOffset: the number of wave groups, then each
// group as its wave bank in the high half and its number in the low half, big-endian. Throws std::invalid_argument for
// more than kMaxWaves groups.
std::vector<uint8_t> WaveList(std::span<const uint32_t> waves);

} // namespace ntdlib::flipper::z2
