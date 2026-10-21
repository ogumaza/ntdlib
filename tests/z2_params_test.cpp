// SPDX-License-Identifier: MIT

// Tests of the host's side of the Z2 start-up's parameters (flipper/z2_params.h): the offsets, the profile's words and
// the wave list that the parameters hold (flipper/wave_list.h).

#include "ntdlib/flipper/z2_params.h"

#include <cstdint>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/flipper/wave_list.h"

namespace z2 = ntdlib::flipper::z2;
using ntdlib::flipper::WaveList;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

int main()
{
    // The profile follows the sound, its handle, the wave count and 64 wave groups.
    Check(z2::kProfileOffset == 0x10c, "the profile's offset");

    // The profile's 53 words in Z2Params' order: the frame's retraces, the 10 sizes, the game's 30 addresses, the two
    // erase functions, the count and the 4 first-scene groups, then the count and the 4 groups for room.
    z2::Profile profile;
    profile.frame_retraces = 1;
    profile.audio_heap_size = 0x180000;
    profile.vi_init = 0x80001000;
    profile.archive_heap_parent = 0x8052112c;
    profile.start_sound = 0x8000abcd;
    profile.erase_bgm_wave = 0x8029ca40;
    profile.first_scene_wave_count = 2;
    profile.first_scene_waves = {0x00010000, 0x00000058, 0, 0};
    profile.room_wave_count = 1;
    profile.room_waves = {0x00000002, 0, 0, 0};
    const std::vector<uint32_t> words = profile.Words();
    Check(words.size() == 53, "the profile has 53 words");
    Check(words.size() == 53 && words[0] == 1 && words[10] == 0x180000 && words[11] == 0x80001000 &&
              words[23] == 0x8052112c && words[40] == 0x8000abcd && words[41] == 0x8029ca40 && words[42] == 0 &&
              words[43] == 2 && words[44] == 0x00010000 && words[45] == 0x58 && words[47] == 0 && words[48] == 1 &&
              words[49] == 2 && words[52] == 0,
          "the profile's words in Z2Params' order");
    // A rip writes the words where the driver's struct has the fields. A console's driver may add fields after them.
    Check(sizeof(z2::Profile) == 4 * words.size(), "the profile's size is that of its words");
    Check(z2::Profile{}.Words() == std::vector<uint32_t>(53, 0), "a profile's fields are 0 by default");

    // The wave list: the count, then each group, big-endian.
    const std::vector<uint32_t> kWaves = {0x00010003, 0x00000007};
    Check(WaveList(kWaves) == std::vector<uint8_t>{0, 0, 0, 2, 0, 1, 0, 3, 0, 0, 0, 7}, "a wave list of two groups");
    Check(WaveList({}) == std::vector<uint8_t>{0, 0, 0, 0}, "an empty wave list");
    const std::vector<uint32_t> kTooMany(65, 0);
    Check(ErrorOf([&] { WaveList(kTooMany); }) == "a sound names 65 waves, more than 64", "too many waves");

    return xsflib::testing::Summary("z2 params");
}
