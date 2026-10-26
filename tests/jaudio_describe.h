// SPDX-License-Identifier: MIT

// The text of every field that the JAudio readers give for a wave, a wave system's scene and a wave system. Both
// aaf_test.cpp and jaudio_test.cpp use these functions. A test compares the texts of two files that should read the
// same.

#pragma once

#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/jaudio/wave_system.h"

namespace ntdlib::testing::jaudio_describe
{

inline std::string Describe(const ntdlib::jaudio::WaveInfo& w)
{
    return std::to_string(static_cast<int>(w.format)) + " " + std::to_string(w.base_key) + " " +
           std::to_string(std::bit_cast<uint32_t>(w.sample_rate)) + " " + std::to_string(w.offset) + " " +
           std::to_string(w.size) + " " + std::to_string(w.loop) + " " + std::to_string(w.loop_start) + " " +
           std::to_string(w.loop_end) + " " + std::to_string(w.sample_count) + " " + std::to_string(w.loop_last) + " " +
           std::to_string(w.loop_penult) + " " + w.archive;
}

inline std::string Describe(const ntdlib::jaudio::WaveScene& scene)
{
    std::string text = " scene " + std::to_string(scene.external_mode) + " dependencies";
    for (const uint32_t number : scene.dependencies)
    {
        text += " " + std::to_string(number);
    }

    const auto list = [&text](const char* name, const std::optional<std::vector<uint32_t>>& words)
    {
        text += std::string(" ") + name + (words ? "" : " null");
        for (const uint32_t word : words.value_or(std::vector<uint32_t>()))
        {
            text += " " + std::to_string(word);
        }
    };

    list("cdf", scene.cdf);
    list("cex", scene.cex);
    list("cst", scene.cst);

    return text + "\n";
}

inline std::string Describe(const ntdlib::jaudio::WaveSystem& ws)
{
    std::string text = "system " + std::to_string(ws.number) + "\n";
    for (const ntdlib::jaudio::WaveGroup& group : ws.groups)
    {
        text += " group " + group.archive;
        for (const uint16_t id : group.waves)
        {
            text += " " + std::to_string(id);
        }
        text += "\n" + Describe(group.scene);
        for (const ntdlib::jaudio::WaveInfo& w : group.archive_waves)
        {
            text += "  archive's wave " + Describe(w) + "\n";
        }
    }

    for (const auto& [id, w] : ws.waves)
    {
        text += " wave " + std::to_string(id) + " " + Describe(w) + "\n";
    }

    return text;
}

} // namespace ntdlib::testing::jaudio_describe
