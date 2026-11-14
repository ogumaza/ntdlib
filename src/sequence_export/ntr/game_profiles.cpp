// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/ntr/game_profiles.h"

#include <cstddef>
#include <cstdint>

namespace ntdlib::sequence_export::ntr
{

namespace
{

// PerformOptions::variables numbers the player's variables 0 to 15 and the global variables 16 to 31.
constexpr uint8_t kGlobalVariables = 16;

} // namespace

void ApplyProfile(const ntdlib::ntr::GameProfile& profile, PerformOptions& perform)
{
    const ntdlib::ntr::GameSettings& settings = profile.settings;
    for (const auto& [player, channels] : settings.channels)
    {
        perform.player_channels[player] = channels;
    }

    perform.track_channels.insert(perform.track_channels.end(), settings.track_channels.begin(),
                                  settings.track_channels.end());
    if (settings.reverb)
    {
        perform.reverb = settings.reverb;
    }

    perform.commands.insert(perform.commands.end(), profile.commands.begin(), profile.commands.end());

    for (std::size_t i = 0; i < settings.variables.local.size(); i++)
    {
        if (const auto& value = settings.variables.local[i])
        {
            perform.variables.try_emplace(static_cast<uint8_t>(i), *value);
        }
    }

    for (std::size_t i = 0; i < settings.variables.global.size(); i++)
    {
        if (const auto& value = settings.variables.global[i])
        {
            perform.variables.try_emplace(static_cast<uint8_t>(kGlobalVariables + i), *value);
        }
    }
}

std::vector<Arrangement> ArrangementsOf(const ntdlib::ntr::GameProfile& profile)
{
    std::vector<Arrangement> arrangements;
    arrangements.reserve(profile.arrangements.size());
    for (const ntdlib::ntr::ProfileArrangement& state : profile.arrangements)
    {
        arrangements.push_back({.suffix = state.suffix, .variables = state.variables, .commands = state.commands});
    }

    return arrangements;
}

ntdlib::ntr::GameProfile ProfileOf(const ArchiveInput& input, const ntdlib::ntr::SequenceRef& sequence)
{
    return ntdlib::ntr::GameProfileOf(input.game_code, input.file_path, static_cast<uint32_t>(input.index_in_file),
                                      ntdlib::ntr::Label(input.archive, sequence));
}

void UseGameProfiles(ArchiveConverter& converter)
{
    converter.game_settings =
        [](const ArchiveInput& input, const ntdlib::ntr::SequenceRef& sequence, PerformOptions& perform)
    {
        ApplyProfile(ProfileOf(input, sequence), perform);
    };
    converter.arrangements = [](const ArchiveInput& input, const ntdlib::ntr::SequenceRef& sequence)
    {
        return ArrangementsOf(ProfileOf(input, sequence));
    };
}

} // namespace ntdlib::sequence_export::ntr
