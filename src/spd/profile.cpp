// SPDX-License-Identifier: MIT

#include "ntdlib/spd/profile.h"

#include <array>
#include <cstddef>

#include "xsflib/binary.h"

#include "ntdlib/spd/player.h"

namespace ntdlib::spd
{

namespace
{

// Super Princess Peach (NTR-ASPE-USA, the release read). The game's code sets the branch flag at 0x02011548, 0x02047EB0
// and 0x020B0138, and to 0 at 0x02011538 and 0x02047EF4. At a level's start, 0x02098FE4 to 0x02099110 ask for 0 or 1
// from the level and the game's state. The image doesn't say which levels play 02_007 and 03_002. The files therefore
// play them first at 0, the driver's start-up value, and again at 1. With the flag at 0, 02_007 plays its opening and
// then waits in the loop of 02_002. That loop tests the flag. Only the flag at 1 reaches 02_003 and 02_004. Likewise
// only the flag at 1 reaches 03_003 to 03_005 from 03_002. The game's driver code on ntdlib's ARM7 plays both pieces
// with the flag at 1 frame for frame as the model does.
constexpr std::array<Variant, 2> kPeachVariants = {{
    {"02_007", 1, "_FLAG1"},
    {"03_002", 1, "_FLAG1"},
}};

constexpr std::array<Section, 6> kPeachSections = {{
    {"02_002", "02_007"},
    {"02_003", "02_007"},
    {"02_004", "02_007"},
    {"03_003", "03_002"},
    {"03_004", "03_002"},
    {"03_005", "03_002"},
}};

constexpr int kStartKind = 2;

// The frames for which RedirectOf() compares two sounds: 2 minutes.
constexpr int kCompareFrames = static_cast<int>(120 / kFrameSeconds);

// Whether the model gives the same writes in every frame for the two songs, until both have been idle for 16 frames.
bool SamePlay(std::span<const uint8_t> data, const std::vector<Block>& blocks, const DriverInfo& info, int a, int b)
{
    const auto start = [&](int song)
    {
        const SongEntry& entry = info.songs.at(static_cast<std::size_t>(song));
        const Sets sets = SetsOf(blocks, info, song);
        const int slot = info.blocks.at(static_cast<std::size_t>(entry.block)).slot;
        Player player(data, *sets.a, sets.b, slot, GameLayout(data));
        player.Post(StartMessage(slot, entry.sound + 1));
        return player;
    };

    Player first = start(a);
    Player second = start(b);
    int idle = 0;
    for (int frame = 0; frame < kCompareFrames && idle <= 16; frame++)
    {
        if (first.Frame() != second.Frame())
        {
            return false;
        }

        idle = first.Busy() || second.Busy() ? 0 : idle + 1;
    }

    return true;
}

} // namespace

Profile ProfileOf(std::string_view game_code)
{
    if (game_code == "ASPE")
    {
        return {kPeachVariants, kPeachSections};
    }

    return {};
}

std::optional<Section> SectionOf(const Profile& profile, std::string_view name)
{
    for (const Section& section : profile.sections)
    {
        if (section.sound == name)
        {
            return section;
        }
    }

    return std::nullopt;
}

std::optional<int> RedirectOf(std::span<const uint8_t> data, const std::vector<Block>& blocks, const DriverInfo& info,
                              int song)
{
    const SongEntry& entry = info.songs.at(static_cast<std::size_t>(song));
    const Block& block = blocks.at(static_cast<std::size_t>(entry.block));
    const std::optional<Sound> sound = block.SoundAt(entry.sound);
    if (!sound)
    {
        return std::nullopt;
    }

    // The start action: a start of a sound of this block or of set A, with no fade-in, on the same player.
    const int action = static_cast<int>((sound->flags >> 20) & 0x3F) - 1;
    if (action < 0)
    {
        return std::nullopt;
    }
    const uint32_t message = xsflib::LoadLe32(&block.Bytes()[kEndActionsOffset + static_cast<std::size_t>(action) * 4]);
    const int set = static_cast<int>((message >> 16) & 0x3F);
    const int slot = info.blocks.at(static_cast<std::size_t>(entry.block)).slot;
    if ((message & 0xFF) != kStartKind || ((message >> 8) & 0xFF) != 0 || (set != 0 && set != slot))
    {
        return std::nullopt;
    }

    const int target_block = set == 0 ? 0 : entry.block;
    const int target_sound = static_cast<int>(message >> 22) - 1;
    for (std::size_t other = 0; other < info.songs.size(); other++)
    {
        const SongEntry& candidate = info.songs[other];
        if (candidate.block != target_block || candidate.sound != target_sound || static_cast<int>(other) == song)
        {
            continue;
        }

        const std::optional<Sound> target = blocks.at(static_cast<std::size_t>(target_block)).SoundAt(target_sound);
        if (!target || PlayerOf(target->flags) != PlayerOf(sound->flags))
        {
            return std::nullopt;
        }

        try
        {
            if (!SamePlay(data, blocks, info, song, static_cast<int>(other)))
            {
                return std::nullopt;
            }
        }
        catch (const NoSetB&)
        {
            return std::nullopt;
        }

        return static_cast<int>(other);
    }

    return std::nullopt;
}

} // namespace ntdlib::spd
