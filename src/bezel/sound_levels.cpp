// SPDX-License-Identifier: MIT

#include "ntdlib/bezel/sound_levels.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/files.h"

namespace ntdlib::bezel
{
namespace
{

namespace fs = std::filesystem;

// The fields of a record that give its level: a pointer to the record's name at +0x0, a pointer to the sound's name at
// +0x10, the level at +0x18, and the bounds of a random change to the level at +0x4C and +0x50. A record without a
// sound's name plays the sound named like the record.
constexpr std::size_t kSoundName = 0x10;
constexpr std::size_t kLevel = 0x18;
constexpr std::size_t kLowestChange = 0x4C;
constexpr std::size_t kHighestChange = 0x50;
constexpr std::size_t kLastField = kHighestChange + 4;

// The value of a field that the record leaves to the default record. The games' default record gives a level of 1, and
// no random change.
constexpr float kUnset = -999999.0f;
constexpr float kDefaultLevel = 1.0f;

constexpr std::array<const char*, 2> kTables = {"s_VoiceDataTable", "s_VoiceDataList"};
constexpr std::array<const char*, 2> kOtherLists = {"s_voiceData_", "s_VoiceDataList_"};

constexpr std::string_view kSheetPrefix = "SoundVoice_";
constexpr std::string_view kVoicePrefix = "SoundVoice_Voice_";
constexpr std::string_view kSheetExtension = ".elf";

float FloatAt(std::span<const uint8_t> record, std::size_t offset)
{
    return std::bit_cast<float>(xsflib::LoadLe32(&record[offset]));
}

// The level at which a record plays its sound. The game adds a random change to the level, and takes anything below 0
// as 0. A rip holds the middle of the change's bounds.
float Level(std::span<const uint8_t> record)
{
    const float level = FloatAt(record, kLevel);
    const float low = FloatAt(record, kLowestChange);
    const float high = FloatAt(record, kHighestChange);
    const float change = low == kUnset || high == kUnset ? 0.0f : (low + high) / 2.0f;
    const float total = (level == kUnset ? kDefaultLevel : level) + change;

    return total > 0.0f ? total : 0.0f;
}

} // namespace

void SoundLevels::AddSheet(const Sheet& sheet)
{
    const std::size_t size = sheet.Symbol("s_DefaultVoiceData").size();
    if (size < kLastField)
    {
        return;
    }

    const auto add_list = [this, &sheet, size](const std::string& list)
    {
        const std::span<const uint8_t> records = sheet.Symbol(list);
        for (std::size_t at = 0; at + size <= records.size(); at += size)
        {
            const std::optional<std::string> key = sheet.StringAt(list, at);
            const std::optional<std::string> name = sheet.StringAt(list, at + kSoundName);
            const std::string sound = name && !name->empty() ? *name : key.value_or("");
            if (sound.empty())
            {
                continue;
            }

            const float level = Level(records.subspan(at, size));
            Records& entry = sounds_[sound];
            if (key && *key == sound)
            {
                entry.named = level;
            }
            else
            {
                ++entry.by_level[level];
            }
        }
    };

    for (const char* table : kTables)
    {
        add_list(table);
    }
    for (const char* prefix : kOtherLists)
    {
        for (const std::string& list : sheet.SymbolsStartingWith(prefix))
        {
            add_list(list);
        }
    }
}

float SoundLevels::LevelOf(std::string_view sound) const
{
    const auto found = sounds_.find(sound);
    if (found == sounds_.end())
    {
        return kDefaultLevel;
    }

    const Records& records = found->second;
    if (records.named)
    {
        return *records.named;
    }

    // The map goes up in level. A tie therefore goes to the higher level.
    float level = kDefaultLevel;
    int most = 0;
    for (const auto& [candidate, count] : records.by_level)
    {
        if (count >= most)
        {
            level = candidate;
            most = count;
        }
    }

    return level;
}

bool IsSoundSheet(std::string_view name)
{
    return name.starts_with(kSheetPrefix) && !name.starts_with(kVoicePrefix) && name.ends_with(kSheetExtension);
}

SoundLevels ReadSoundLevels(const fs::path& folder)
{
    SoundLevels levels;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!IsSoundSheet(xsflib::Utf8(it->path().filename())))
        {
            continue;
        }

        std::vector<uint8_t> bytes;
        Sheet sheet;
        if (xsflib::ReadFile(it->path(), bytes) && !Sheet::Parse(std::move(bytes), sheet))
        {
            levels.AddSheet(sheet);
        }
    }

    return levels;
}

namespace
{

// Whether `folder` holds a sound sheet.
bool HoldsSoundSheet(const fs::path& folder)
{
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        if (IsSoundSheet(xsflib::Utf8(it->path().filename())))
        {
            return true;
        }
    }

    return false;
}

} // namespace

std::optional<fs::path> FindSoundSheets(const fs::path& archive)
{
    const fs::path sheets = fs::path("Data") / "SheetBin";
    fs::path folder = archive.parent_path().empty() ? fs::path(".") : archive.parent_path();
    for (int up = 0; up < 4; ++up)
    {
        if (HoldsSoundSheet(folder / sheets))
        {
            return folder / sheets;
        }

        std::vector<fs::path> below;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            below.push_back(it->path());
        }

        std::sort(below.begin(), below.end());
        for (const fs::path& path : below)
        {
            if (HoldsSoundSheet(path / sheets))
            {
                return path / sheets;
            }
        }

        folder = folder / "..";
    }

    return std::nullopt;
}

} // namespace ntdlib::bezel
