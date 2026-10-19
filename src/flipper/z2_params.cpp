// SPDX-License-Identifier: MIT

#include "ntdlib/flipper/z2_params.h"

namespace ntdlib::flipper::z2
{

std::vector<uint32_t> Profile::Words() const
{
    std::vector<uint32_t> words = {frame_retraces,      max_heaps,           system_heap_size,
                                   aram_audio_size,     aram_graph_size,     aram_priorities,
                                   aram_buffer_size,    command_heap_size,   archive_heap_size,
                                   zelda_heap_margin,   audio_heap_size,     vi_init,
                                   vi_wait_for_retrace, get_current_thread,  get_thread_priority,
                                   create_root_heap,    create_exp_heap,     create_solid_heap,
                                   create_aram,         set_aram_buffer,     create_thread_switch,
                                   create_command_heap, create_archive_heap, archive_heap_parent,
                                   create_assert_heap,  create_zelda_heap,   get_free_size,
                                   become_current_heap, system_heap,         alloc,
                                   thread_ctor,         create_disc_thread,  audio_heap,
                                   audio_setup,         init_flag,           audio_frame,
                                   load_se_wave,        load_bgm_wave,       wave_status,
                                   audio_manager,       start_sound};
    words.insert(words.end(), {erase_bgm_wave, erase_se_wave, first_scene_wave_count});
    words.insert(words.end(), first_scene_waves.begin(), first_scene_waves.end());
    words.push_back(room_wave_count);
    words.insert(words.end(), room_waves.begin(), room_waves.end());
    return words;
}

} // namespace ntdlib::flipper::z2
