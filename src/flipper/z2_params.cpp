// SPDX-License-Identifier: MIT

#include "ntdlib/flipper/z2_params.h"

#include <bit>
#include <cstdint>
#include <vector>

#include "xsflib/bytes.h"

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
    words.insert(words.end(), {music_manager, set_child_volume, set_port_data, get_port_data});

    return words;
}

std::vector<uint8_t> PieceState::Bytes() const
{
    std::vector<uint32_t> words = {muted_children, partial_children};
    for (const float volume : partial_volumes)
    {
        words.push_back(std::bit_cast<uint32_t>(volume));
    }

    words.push_back(music_status);
    words.push_back(section_statuses);
    while (!words.empty() && words.back() == 0)
    {
        words.pop_back();
    }

    std::vector<uint8_t> bytes(4 * words.size());
    for (std::size_t i = 0; i < words.size(); i++)
    {
        xsflib::StoreBe32(bytes.data() + 4 * i, words[i]);
    }

    return bytes;
}

PieceState PieceStateOf(const jaudio::MusicState& state)
{
    PieceState piece;
    for (uint32_t child = 0; child < kMaxPartialChildren; child++)
    {
        const float volume = state.Volume(child);
        if (volume == 0.0f)
        {
            piece.muted_children |= 1u << child;
        }
        else if (volume != 1.0f)
        {
            piece.partial_children |= 1u << child;
            piece.partial_volumes[child] = volume;
        }
    }

    if (state.status_port >= 0)
    {
        piece.music_status =
            kMusicStatusSet | (state.status_later ? kMusicStatusLater : 0) | static_cast<uint32_t>(state.status_port);
    }

    for (std::size_t i = 0; i < state.section_statuses.size(); i++)
    {
        const jaudio::SectionStatus& s = state.section_statuses[i];
        piece.section_statuses |= static_cast<uint32_t>(s.section << 8 | s.status) << (16 * i);
    }

    return piece;
}

} // namespace ntdlib::flipper::z2
