// SPDX-License-Identifier: MIT

// Tests of Luigi's Mansion's states of its music (jaudio/luigis_mansion_states.h) against what the game's code set in
// Dolphin (luigis_mansion_runs.h).

#include "ntdlib/jaudio/luigis_mansion_states.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "xsflib/testing/check.h"

#include "luigis_mansion_runs.h"

namespace luigis_mansion = ntdlib::jaudio::luigis_mansion;
using luigis_mansion::kBossStates;
using luigis_mansion::kSingingStates;
using luigis_mansion::SingingPlace;
using luigis_mansion_runs::BossRun;
using luigis_mansion_runs::kBossRuns;
using xsflib::testing::Check;

namespace
{

// The state of the table for a run's piece and number, or nullptr.
const luigis_mansion::BossState* Find(uint32_t sound, uint8_t state)
{
    for (const luigis_mansion::BossState& boss : kBossStates)
    {
        if (boss.sound == sound && boss.state == state)
        {
            return &boss;
        }
    }

    return nullptr;
}

bool SameVolumes(const luigis_mansion::BossState& boss, std::string_view volumes)
{
    for (unsigned child = 0; child < 16; child++)
    {
        if (boss.Volume(child) != (volumes[child] == '1' ? 1.0f : 0.0f))
        {
            return false;
        }
    }

    return true;
}

} // namespace

int main()
{
    // Every state of the table has a run, and every run's state is in the table.
    Check(kBossStates.size() == std::size(kBossRuns), "the table contains the 10 states that the runs read");
    for (const BossRun& run : kBossRuns)
    {
        const luigis_mansion::BossState* boss = Find(run.sound, run.state);
        Check(boss != nullptr && SameVolumes(*boss, run.volumes), "the volumes of each run's state");
        // TelesaBattle's tempo comes from 0x8018c77c, not from the state function. The runs didn't call it.
        if (boss != nullptr && boss->sound != 0x80000031)
        {
            Check(std::bit_cast<uint32_t>(boss->tempo) == run.tempo_bits, "the tempo of each run's state");
        }
    }

    // TelesaBattle's state 1: the tempo that 0x8018c77c(1, b) computes in single precision, and the row's tempo at the
    // first count.
    Check(std::bit_cast<uint32_t>(luigis_mansion::TelesaBattleTempo(1)) == 0x3fb35ee4 &&
              luigis_mansion::TelesaBattleTempo(15) == 1.0f && luigis_mansion::TelesaBattleTempo(200) == 1.0f,
          "TelesaBattle's state 1 plays at 1 + 0.43 x (15 - min(b, 15)) / 15 of the tempo");
    const luigis_mansion::BossState* telesa = Find(0x80000031, 1);
    Check(telesa != nullptr &&
              telesa->tempo == luigis_mansion::TelesaBattleTempo(luigis_mansion::kTelesaBattleFirstCount),
          "TelesaBattle's state 1 takes the tempo of the first count");
    Check(Find(0x80000031, 0) != nullptr && Find(0x80000031, 0)->tempo == 1.0f, "TelesaBattle starts at its tempo");

    // Each piece has a state 0, and no piece has a state twice.
    std::set<std::pair<uint32_t, uint8_t>> seen;
    for (const luigis_mansion::BossState& boss : kBossStates)
    {
        seen.insert({boss.sound, boss.state});
    }
    Check(seen.size() == kBossStates.size(), "each state appears once");
    for (const uint32_t sound : {0x8000002bu, 0x8000002eu, 0x80000031u, 0x80000052u})
    {
        Check(Find(sound, 0) != nullptr, "each boss piece has its start's state");
    }

    // LuigiSings: at 100 HP, the game plays the tune on child 4 and mutes children 5 to 7. With the byte at 0x804d91b9
    // held at 1, it mutes children 4 to 7 (the run with the title's start patched to LuigiSings).
    const luigis_mansion::SingingState full{SingingPlace::kHallway, 100};
    const luigis_mansion::SingingState muted{SingingPlace::kHallway, 100, false, true};
    Check(full.TuneChild() == 4 && full.Volume(4) == 1.0f && full.Volume(5) == 0.0f && full.Volume(7) == 0.0f,
          "at 100 HP the tune is child 4");
    Check(muted.TuneChild() == -1 && muted.Volume(4) == 0.0f && muted.Volume(5) == 0.0f && muted.Volume(6) == 0.0f &&
              muted.Volume(7) == 0.0f,
          "with the humming muted, children 4 to 7 are silent");
    Check(muted.Playing() == (full.Playing() & ~0x00f0) && muted.Tempo() == full.Tempo(),
          "the humming-free state keeps the accompaniment and the tempo");

    // The table's states with the humming muted: one at 100 HP for each place with an accompaniment.
    std::set<SingingPlace> muted_places;
    for (const luigis_mansion::SingingState& singing : kSingingStates)
    {
        if (singing.humming_muted)
        {
            Check(singing.health == 100 && !singing.shrunk && luigis_mansion::AccompanimentOf(singing.place) != 0,
                  "a state with the humming muted has an accompaniment at 100 HP");
            muted_places.insert(singing.place);
        }
    }
    Check(muted_places == std::set<SingingPlace>{SingingPlace::kHallway, SingingPlace::kRoom, SingingPlace::kOutdoors},
          "the hallway, the rooms and outdoors have a state with the humming muted");

    // The labels of the states, which the names of the files are built from.
    constexpr std::string_view kSingingLabels[] = {
        "hallway 100 HP",
        "hallway 70 HP",
        "hallway 40 HP",
        "room 100 HP",
        "room 70 HP",
        "room 40 HP",
        "outdoors 100 HP",
        "outdoors 70 HP",
        "outdoors 40 HP",
        "unaccompanied 100 HP",
        "unaccompanied 70 HP",
        "unaccompanied 40 HP",
        "lit room",
        "hallway 100 HP shrunk",
        "room 100 HP shrunk",
        "outdoors 100 HP shrunk",
        "unaccompanied 100 HP shrunk",
        "hallway 100 HP no humming",
        "room 100 HP no humming",
        "outdoors 100 HP no humming",
    };
    bool same_labels = kSingingStates.size() == std::size(kSingingLabels);
    for (std::size_t i = 0; same_labels && i < kSingingStates.size(); i++)
    {
        same_labels = luigis_mansion::LabelOf(kSingingStates[i]) == kSingingLabels[i];
    }
    Check(same_labels, "each state of LuigiSings is labelled by its place, its health and its flags");
    Check(luigis_mansion::LabelOf(*Find(0x8000002b, 0)).empty() &&
              luigis_mansion::LabelOf(*Find(0x8000002b, 2)) == "state 2",
          "a boss state from 1 up is labelled by its number, and state 0 has no label");

    // The places' rooms and their packed sound information.
    Check(luigis_mansion::RoomSound(luigis_mansion::RoomOf(SingingPlace::kHallway)) == (10 | 7 << 8) &&
              luigis_mansion::RoomSound(luigis_mansion::RoomOf(SingingPlace::kOutdoors)) == (2 << 6 | 1 << 8) &&
              luigis_mansion::RoomOf(SingingPlace::kUnaccompanied).room == 19,
          "a place's room and its fields of the room table");

    return 0;
}
