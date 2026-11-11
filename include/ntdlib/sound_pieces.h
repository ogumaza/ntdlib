// SPDX-License-Identifier: MIT

// Music that a game plays as several sequences together. A game can start several sequences in one sound frame and play
// them as one piece. Games that play the music of an older console's sound chip play each channel as a sequence. The
// labels of a piece's sequences then share one name, and each ends with its channel's suffix.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ntdlib
{

// A piece that a game plays as several sequences: its name, and its sequences' sounds, in the order in which the game
// starts them.
struct SoundPiece
{
    std::string name;
    std::vector<uint32_t> sounds;
};

// The suffixes of Mighty Gunvolt Burst's music on the 3DS, one for each channel of the NES's sound chip: the DPCM
// channel, the noise, the two pulse channels and the triangle. The game's table at 0x49f000 gives each of its 27
// pieces' five sounds in that order. The function at 0x2d1b24 starts a piece. It calls 0x2cd2b0 for each channel in
// turn, in one sound frame.
constexpr std::array<std::string_view, 5> kMightyGunvoltBurstChannels = {"_DPCM", "_NOISE", "_P1", "_P2", "_TRI"};

// The suffixes of Mega Man 9's music on the Wii, one for each channel of the NES's sound chip: the two pulse channels,
// the triangle and the noise. The game's table of its 33 pieces (0x802805c0) gives each piece's four sounds in that
// order. The function at 0x800d5564 starts the four in that order, with one call for each. The four start in the same
// sound frame.
constexpr std::array<std::string_view, 4> kMegaMan9Channels = {"_S1", "_S2", "_TR", "_NO"};

// The pieces of `archive`, in the order of their first sounds. A piece's sequences have one name followed by each of
// `suffixes`, in that order. The piece takes that name. Of two sequences with one label, the first counts. `Archive`
// is a sound archive whose Sounds() give each sound's name and type, such as sar::SoundArchive or rsar::SoundArchive.
template <typename Archive>
std::vector<SoundPiece> FindSoundPieces(const Archive& archive, std::span<const std::string_view> suffixes)
{
    std::vector<SoundPiece> pieces;
    if (suffixes.empty())
    {
        return pieces;
    }

    // The sequences by label. A later sequence with an earlier one's label doesn't replace it.
    const auto& sounds = archive.Sounds();
    const auto is_sequence = [&sounds](std::size_t i)
    {
        return sounds[i].type == decltype(sounds[i].type)::kSequence;
    };
    std::map<std::string, uint32_t, std::less<>> sequences;
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        if (is_sequence(i))
        {
            sequences.emplace(sounds[i].name, static_cast<uint32_t>(i));
        }
    }

    // Each piece starts at its first sequence. Every one of its labels has to be there.
    const std::string_view first = suffixes.front();
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const std::string& label = sounds[i].name;
        if (!is_sequence(i) || label.size() <= first.size() || !label.ends_with(first))
        {
            continue;
        }

        SoundPiece piece{label.substr(0, label.size() - first.size()), {}};
        for (const std::string_view suffix : suffixes)
        {
            const auto found = sequences.find(piece.name + std::string(suffix));
            if (found == sequences.end())
            {
                break;
            }

            piece.sounds.push_back(found->second);
        }

        if (piece.sounds.size() == suffixes.size() && piece.sounds.front() == i)
        {
            pieces.push_back(std::move(piece));
        }
    }

    return pieces;
}

// The piece of `pieces` whose first sound is `sound`, or null.
inline const SoundPiece* PieceStartingAt(std::span<const SoundPiece> pieces, std::size_t sound)
{
    for (const SoundPiece& piece : pieces)
    {
        if (!piece.sounds.empty() && piece.sounds.front() == sound)
        {
            return &piece;
        }
    }

    return nullptr;
}

} // namespace ntdlib
