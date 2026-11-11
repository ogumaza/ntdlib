// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/jac_effects.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/text.h"

#include "ntdlib/dol.h"

namespace ntdlib::jaudio
{
namespace
{

// The bytes of a row of the catalogue and of an action's entry.
constexpr std::size_t kRow = 20;
constexpr std::size_t kAction = 6;

} // namespace

JacCatalogue::JacCatalogue(const Dol& dol, uint32_t table, uint32_t count, uint32_t event_offsets)
    : dol_(&dol), rows_(dol.At(table)), offsets_(dol.At(event_offsets)), count_(count)
{
    if (rows_.size() / kRow < count || offsets_.size() < kEventTypes * 4)
    {
        throw xsflib::FormatError("main.dol's catalogue of effects runs past its section");
    }

    actions_ = offsets_.subspan(kEventTypes * 4);
}

JacCatalogue::Row JacCatalogue::RowAt(uint32_t i) const
{
    const uint8_t* row = rows_.data() + std::size_t{i} * kRow;
    const uint32_t name_address = xsflib::LoadBe32(row + 8);
    const std::span<const uint8_t> text = dol_->At(name_address);
    const auto end = std::find(text.begin(), text.end(), uint8_t{0});
    if (end == text.end())
    {
        throw xsflib::FormatError("main.dol's name of an effect at " + xsflib::Hex(name_address, 8) + " has no end");
    }

    return {.number = xsflib::LoadBe32(row + 4),
            .name = std::string(text.begin(), end),
            .kind = static_cast<int32_t>(xsflib::LoadBe32(row + 16))};
}

uint32_t JacCatalogue::FirstEntry(uint32_t type) const
{
    return xsflib::LoadBe32(offsets_.data() + 4 * std::size_t{type});
}

std::optional<uint16_t> JacCatalogue::EffectOf(std::size_t entry) const
{
    if (actions_.size() / kAction <= entry)
    {
        return std::nullopt;
    }

    return xsflib::LoadBe16(actions_.data() + entry * kAction + 4);
}

std::vector<JacEffectSound> ListJacEffects(const Dol& dol, const JacEffectRules& rules,
                                           const std::function<std::string(uint32_t id)>& name_of_other)
{
    if (!rules.table)
    {
        return {};
    }

    const JacCatalogue catalogue(dol, rules.table, rules.count, rules.event_offsets);
    std::vector<JacEffectSound> effects;
    std::set<uint32_t> ids;
    const auto add = [&](uint32_t id, std::string name)
    {
        if (std::ranges::find(rules.left_out, id) == rules.left_out.end() && ids.insert(id).second)
        {
            effects.push_back({id, std::move(name)});
        }
    };

    std::set<uint16_t> event_effects;
    for (uint32_t i = 0; i < catalogue.Count(); i++)
    {
        JacCatalogue::Row row = catalogue.RowAt(i);
        if (row.kind == JacCatalogue::kSystemRow || row.kind == JacCatalogue::kPlayerRow)
        {
            add(JacEffectId(row.kind == JacCatalogue::kSystemRow ? JacEffect::kSystem : JacEffect::kPlayer, row.number),
                std::move(row.name));
            continue;
        }

        if (row.kind < 1 || row.kind >= static_cast<int32_t>(JacCatalogue::kEventTypes))
        {
            throw xsflib::FormatError("main.dol's catalogue of effects has a row of unknown kind");
        }

        // An event of the sounding type starts another effect as the game creates it. The rip plays the effects of
        // that type on an event of the quiet type, by the same entry in the table of actions.
        auto type = static_cast<uint32_t>(row.kind);
        const uint32_t entry = catalogue.FirstEntry(type) + row.number;
        const std::optional<uint16_t> effect = catalogue.EffectOf(entry);
        if (!effect)
        {
            throw xsflib::FormatError("main.dol's table of actions runs past its section");
        }

        if (!event_effects.insert(*effect).second)
        {
            continue;
        }

        if (type == rules.sounding_event)
        {
            type = rules.quiet_event;
        }

        add(JacEffectId(JacEffect::kEvent, type << 12 | (entry - catalogue.FirstEntry(type))), std::move(row.name));
    }

    for (const uint32_t id : rules.other_effects)
    {
        add(id, name_of_other(id));
    }

    return effects;
}

} // namespace ntdlib::jaudio
