// SPDX-License-Identifier: MIT

// Checks what the rippers' game modes share of three JAudio games: Donkey Kong Jungle Beat's states
// (ntdlib/jaudio/jungle_beat_states.h), Pikmin 2's pieces and the drivers' table of them
// (ntdlib/jaudio/pikmin2_pieces.h), and Jac's effect IDs and catalogue of effects (ntdlib/jaudio/jac_effects.h), on a
// main.dol built here. The bits and the table's words follow the layouts of the drivers' sources.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dol.h"
#include "ntdlib/jaudio/jac_effects.h"
#include "ntdlib/jaudio/jungle_beat_states.h"
#include "ntdlib/jaudio/pikmin2_pieces.h"
#include "ntdlib/jaudio/wind_waker_ports.h"
#include "ntdlib/testing/dol_files.h"

namespace
{

namespace jaudio = ntdlib::jaudio;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

void TestJungleBeatStates()
{
    Check(std::size(jaudio::kJungleBeatStates) == 101 && jaudio::kJungleBeatWiiStates.size() == 102,
          "Jungle Beat has 101 states on the GameCube and one more on the Wii");
    Check(std::equal(std::begin(jaudio::kJungleBeatStates), std::end(jaudio::kJungleBeatStates),
                     jaudio::kJungleBeatWiiStates.begin(),
                     [](const jaudio::JungleBeatState& a, const jaudio::JungleBeatState& b)
                     {
                         return a.piece == b.piece && a.bits == b.bits && std::string_view(a.suffix) == b.suffix &&
                                a.children == b.children;
                     }),
          "the Wii's states start with the GameCube's");
    const jaudio::JungleBeatState& course = jaudio::kJungleBeatWiiStates.back();
    Check(course.piece == 0x0100002c && course.bits == 1u << 18 && std::string_view(course.suffix) == "state1" &&
              course.children == 0xfff2,
          "the Wii adds state 1 of JA_BGM_COURSE_SELECT");

    // The drivers read a combo's layer in bits 16 and 17, a state of the second table in bits 18 to 20, and the
    // Helibird in bit 21.
    Check((jaudio::Combo(2) | jaudio::TableState(3) | jaudio::kHelibird) == 0x2e0000,
          "the state's bits are those that the drivers read");
    bool fields = true;
    for (const jaudio::JungleBeatState& state : jaudio::kJungleBeatWiiStates)
    {
        const uint32_t layer = state.bits >> jaudio::kComboLayerShift & 3;
        const uint32_t table_state = state.bits >> jaudio::kTableStateShift & 7;
        fields = fields && state.bits != 0 && (state.bits & ~0x3f0000u) == 0 && (state.piece & 0xff0000u) == 0 &&
                 (layer == 0 || table_state == 0) && *state.suffix;
    }
    Check(fields, "each state sets a combo's layer or a table's state, with or without the Helibird");
    Check(std::is_sorted(std::begin(jaudio::kJungleBeatStates), std::end(jaudio::kJungleBeatStates),
                         [](const jaudio::JungleBeatState& a, const jaudio::JungleBeatState& b)
                         { return a.piece < b.piece; }),
          "the GameCube's states come in the order of their pieces");
}

void TestPikmin2Pieces()
{
    const std::span<const jaudio::Pikmin2Piece> pieces = jaudio::kPikmin2Pieces;
    Check(pieces.size() == 85, "Pikmin 2 has 85 pieces and variants");
    Check(jaudio::Pikmin2PieceOf(0x80000000) == &pieces[0] && jaudio::Pikmin2PieceOf(0x80000054) == &pieces[84],
          "a sequence's ID gives the piece at its place");
    Check(!jaudio::Pikmin2PieceOf(0x80000055) && !jaudio::Pikmin2PieceOf(0x00000000) &&
              !jaudio::Pikmin2PieceOf(0xc0000000),
          "an ID past the table or of another type of sound gives no piece");
    Check(pieces[0].Name() == "2pbattle" && pieces[84].Name() == "l_boss-11-near",
          "a piece is named after its file, and a variant after its label");

    // driver/pikmin2.c's table: the count, then for each piece the kind with the start point from bit 8 and the beat
    // interval from bit 16, the four words of the sound information, and the two names in 24 bytes each.
    std::vector<uint32_t> words = {0x12345678};
    jaudio::AppendPikmin2Pieces(words);
    constexpr std::size_t kPieceWords = 1 + 4 + 6 + 6;
    Check(words.size() == 2 + 85 * kPieceWords && words[0] == 0x12345678 && words[1] == 85,
          "the table follows the words before it and starts with its count");
    const std::vector<uint32_t> battle = {
        4,          0x1f00,     0x7f010000, 0x3f800000, 0x32000000,    // kBattle, initMainBgm's sound information
        0x32706261, 0x74746c65, 0x2e626d73, 0,          0,          0, // "2pbattle.bms"
        0,          0,          0,          0,          0,          0, // no conductor
    };
    Check(std::equal(battle.begin(), battle.end(), words.begin() + 2), "the first piece's words");
    const auto last = words.begin() + 2 + 84 * static_cast<std::ptrdiff_t>(kPieceWords);
    const std::vector<uint32_t> near = {
        8 | 11 << 8 | 60 << 16,
        0,
        0x7f020000,
        0x3f800000,
        0x28000000, // kBigBossNear at part 11, initBossBgm's
        0x6c5f626f,
        0x73732e62,
        0x6d730000,
        0,
        0,
        0, // "l_boss.bms"
    };
    Check(std::equal(near.begin(), near.end(), last), "the last variant's words");
}

// A main.dol with one data section at 0x80100000 that holds names of effects, a catalogue of `kinds.size()` rows and
// the table of event offsets with its actions. Row i gives number numbers[i] and kind kinds[i]. Event type 1 starts at
// entry 0, type 2 at entry 3 and type 7 at entry 6. Entries 1 and 5 play effect 0x10, and entry 10 effect 0x22.
// `last_name` is the name of the last row. It ends the section, with no zero after it when `unended`.
std::vector<uint8_t> CatalogueDol(const std::vector<uint32_t>& numbers, const std::vector<int32_t>& kinds,
                                  bool unended = false)
{
    constexpr uint32_t kBase = 0x80100000;
    constexpr std::size_t kHeader = 0x100;
    const std::vector<std::string> names = {"stop", "fanfare", "whistle", "boom", "idle", "door"};
    std::vector<uint8_t> data(0x300);
    std::vector<uint32_t> name_at;
    std::size_t at = 0;
    for (const std::string& name : names)
    {
        name_at.push_back(kBase + static_cast<uint32_t>(at));
        std::copy(name.begin(), name.end(), data.begin() + static_cast<std::ptrdiff_t>(at));
        at += name.size() + 1;
    }

    for (std::size_t i = 0; i < kinds.size(); i++)
    {
        uint8_t* row = &data[0x100 + 20 * i];
        xsflib::StoreBe32(row + 4, numbers[i]);
        xsflib::StoreBe32(row + 8, i + 1 == kinds.size() && unended ? kBase + 0x2fe : name_at[i % names.size()]);
        xsflib::StoreBe32(row + 16, static_cast<uint32_t>(kinds[i]));
    }

    xsflib::StoreBe32(&data[0x200 + 4 * 1], 0);
    xsflib::StoreBe32(&data[0x200 + 4 * 2], 3);
    xsflib::StoreBe32(&data[0x200 + 4 * 7], 6);
    for (const auto [entry, effect] : {std::pair<std::size_t, uint16_t>{1, 0x10}, {5, 0x10}, {10, 0x22}})
    {
        xsflib::StoreBe16(&data[0x220 + 6 * entry + 4], effect);
    }

    if (unended)
    {
        data[0x2fe] = 'x';
        data[0x2ff] = 'y';
    }

    std::vector<uint8_t> dol(kHeader);
    ntdlib::testing::SetDolSection(dol, 7, kHeader, kBase, static_cast<uint32_t>(data.size()));
    dol.insert(dol.end(), data.begin(), data.end());
    return dol;
}

void TestJacEffectIds()
{
    Check(jaudio::JacEffectId(jaudio::JacEffect::kSystem, 25) == 0x81001900 &&
              jaudio::JacEffectId(jaudio::JacEffect::kPlayer, 0x1800c) == 0x82800c00 &&
              jaudio::JacEffectId(jaudio::JacEffect::kEvent, 7 << 12 | 4) == 0x83700401 &&
              jaudio::JacEffectId(jaudio::JacEffect::kFormation, 0) == 0x84000000,
          "an effect's ID holds its kind, its number's 16 bits and the file that plays it");
    Check(jaudio::IsJacEffect(0x83700401) && jaudio::IsJacEffect(0x81001900) && !jaudio::IsJacEffect(0x80000002),
          "the effects' files come before the pieces");
}

void TestJacCatalogue()
{
    using jaudio::JacEffect;
    using jaudio::JacEffectId;
    const std::vector<uint8_t> file =
        CatalogueDol({0, 5, 0x800c, 1, 4, 2}, {jaudio::JacCatalogue::kSystemRow, jaudio::JacCatalogue::kSystemRow,
                                               jaudio::JacCatalogue::kPlayerRow, 1, 7, 2});
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(file);

    const jaudio::JacCatalogue catalogue(dol, 0x80100100, 6, 0x80100200);
    const jaudio::JacCatalogue::Row row = catalogue.RowAt(4);
    Check(catalogue.Count() == 6 && row.number == 4 && row.name == "idle" && row.kind == 7,
          "a row gives its number, its effect's name and its kind");
    Check(catalogue.FirstEntry(2) == 3 && catalogue.EffectOf(5) == 0x10 && catalogue.EffectOf(10) == 0x22,
          "an event type's first entry, and the effect of an action");
    Check(!catalogue.EffectOf((0x300 - 0x220) / 6), "an action past the section plays nothing");

    const uint32_t other[] = {JacEffectId(JacEffect::kSystem, 5), JacEffectId(JacEffect::kFormation, 0)};
    const uint32_t left_out[] = {JacEffectId(JacEffect::kSystem, 0)};
    jaudio::JacEffectRules rules = {.table = 0x80100100,
                                    .count = 6,
                                    .event_offsets = 0x80100200,
                                    .other_effects = other,
                                    .left_out = left_out,
                                    .sounding_event = 7,
                                    .quiet_event = 1};
    const auto name_of_other = [](uint32_t id)
    {
        return "other-" + std::to_string(id >> 24 & 0x7f);
    };
    const std::vector<jaudio::JacEffectSound> effects = jaudio::ListJacEffects(dol, rules, name_of_other);
    const std::vector<std::pair<uint32_t, std::string>> expected = {
        {JacEffectId(JacEffect::kSystem, 5), "fanfare"},       {JacEffectId(JacEffect::kPlayer, 0x800c), "whistle"},
        {JacEffectId(JacEffect::kEvent, 1 << 12 | 1), "boom"}, {JacEffectId(JacEffect::kEvent, 1 << 12 | 10), "idle"},
        {JacEffectId(JacEffect::kFormation, 0), "other-4"},
    };
    Check(std::ranges::equal(effects, expected,
                             [](const jaudio::JacEffectSound& e, const std::pair<uint32_t, std::string>& x)
                             { return e.id == x.first && e.name == x.second; }),
          "the list leaves out an effect that the rules leave out, a repeated ID and an event's repeated effect, and "
          "plays the sounding type's effects on the quiet type's entries");

    rules.table = 0;
    Check(jaudio::ListJacEffects(dol, rules, name_of_other).empty(), "a game without a catalogue lists no effects");

    const auto error = [&](const std::vector<uint8_t>& bytes, uint32_t table, uint32_t count)
    {
        return ErrorOf<xsflib::FormatError>(
            [&]
            {
                const ntdlib::Dol other_dol = ntdlib::Dol::ParseMain(bytes);
                jaudio::JacEffectRules damaged = rules;
                damaged.table = table;
                damaged.count = count;
                jaudio::ListJacEffects(other_dol, damaged, name_of_other);
            });
    };
    Check(error(file, 0x80100100, 40) == "main.dol's catalogue of effects runs past its section",
          "a catalogue that runs past its section");
    Check(error(file, 0x80200000, 1) == "main.dol's catalogue of effects runs past its section",
          "a catalogue outside the DOL");
    Check(error(CatalogueDol({1}, {9}), 0x80100100, 1) == "main.dol's catalogue of effects has a row of unknown kind",
          "a row of unknown kind");
    Check(error(CatalogueDol({40}, {2}), 0x80100100, 1) == "main.dol's table of actions runs past its section",
          "an event's action past the section");
    Check(error(CatalogueDol({0, 5}, {-2, -2}, true), 0x80100100, 2) ==
              "main.dol's name of an effect at 0x801002fe has no end",
          "a name without its end");
}

void TestWindWakerPorts()
{
    const std::vector<jaudio::PortEffect> effects = jaudio::WindWakerPortEffects();
    Check(effects.size() == 36, "The Wind Waker has 30 effects and 6 pieces that read a port");
    Check(effects.front().id == 0x2803 && effects.front().port == 6 && effects.front().values.size() == 37 &&
              effects.front().values.back() == 68,
          "a step reads the ground's material at port 6");
    Check(effects[29].id == 0x592d && effects[30].id == 0x80000011 && effects[30].port == 9 &&
              effects.back().id == 0x80000004 && effects.back().port == 11 && effects.back().values.size() == 1,
          "the music's statuses come after the effects");
}

} // namespace

int main()
{
    TestJungleBeatStates();
    TestPikmin2Pieces();
    TestJacEffectIds();
    TestJacCatalogue();
    TestWindWakerPorts();

    return xsflib::testing::Summary("jaudio_games");
}
