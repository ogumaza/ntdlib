// SPDX-License-Identifier: MIT

// Tests of the effects that known 3DS games put on the aux buses (ntdlib/ctr/snd/game_effects.h): the lookups by
// program ID and by the sound archive's SHA-1, the effects of some sequences alone, another build of a known archive,
// and the effects while an archive's sound plays. Each game's code gives the values, at the addresses that
// src/ctr/snd/game_effects.cpp cites.

#include <cstdint>
#include <string_view>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ctr/snd/game_effects.h"
#include "ntdlib/ctr/snd/starting_tracks.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"

namespace
{

namespace sar_files = ntdlib::testing::sar_files;

using ntdlib::ctr::snd::ArchiveEffects;
using ntdlib::ctr::snd::AuxEffect;
using ntdlib::ctr::snd::AuxEffectLists;
using ntdlib::ctr::snd::EffectsWhilePlaying;
using ntdlib::ctr::snd::FindArchiveEffects;
using ntdlib::ctr::snd::FindGameEffects;
using ntdlib::ctr::snd::FindOtherBuild;
using ntdlib::ctr::snd::GameEffectOf;
using ntdlib::ctr::snd::GameEffects;
using ntdlib::ctr::snd::kPokemonXFieldReverb;
using ntdlib::ctr::snd::SequenceStart;

constexpr uint8_t kArchiveEffects = SequenceStart::kArchiveEffects;

// The SHA-1s of the sound archives that the tests name.
constexpr std::string_view kPokemonX = "1dd919adf21565f9d9399907f49d610068685cda";
constexpr std::string_view kMajorasMask3D = "35b72405134e662e160c950c62509cb8a84b3e47";
constexpr std::string_view kMiiPlaza = "33b514ee80f6a6df3034bc8774235e40dfa52d1c";
constexpr std::string_view kFlowerTown = "331b572b09819830436fe8a09673b22fc6f7bdae";
constexpr std::string_view kNewLeaf = "54f0fbc801c85ea2035219923aa4cd955e6f4cc7";
constexpr std::string_view kHappyHomeDesigner = "8c07814daf28befb015cc97dd2c66699254035be";
constexpr std::string_view kLuigisMansion = "03cdddad0cb59b6f90dd7cc6727873701f4300b9";
constexpr std::string_view kUnknown = "0000000000000000000000000000000000000000";

// Whether `effects` puts a reverb on bus A.
bool ReverbOnBusA(const AuxEffectLists& effects)
{
    for (const AuxEffect& effect : effects[0])
    {
        if (effect.IsReverb())
        {
            return true;
        }
    }

    return false;
}

void TestKinds()
{
    AuxEffect firmware_reverb;
    firmware_reverb.kind = AuxEffect::Kind::kFirmwareReverb;
    AuxEffect firmware_delay;
    firmware_delay.kind = AuxEffect::Kind::kFirmwareDelay;
    AuxEffect delay;
    delay.kind = AuxEffect::Kind::kDelay;

    XSFLIB_CHECK(firmware_reverb.IsReverb() && firmware_reverb.InFirmware());
    XSFLIB_CHECK(!firmware_delay.IsReverb() && firmware_delay.InFirmware());
    XSFLIB_CHECK(!delay.IsReverb() && !delay.InFirmware());
    XSFLIB_CHECK(AuxEffect{}.IsReverb() && !AuxEffect{}.InFirmware());
    XSFLIB_CHECK(!AuxEffect::Known(0) && AuxEffect::Known(1) && AuxEffect::Known(7) && !AuxEffect::Known(8));
}

// Each release of the table by program ID finds its game. The Japanese release of Ocarina of Time 3D finds the same
// entry as the North American one.
void TestProgramIds()
{
    const GameEffects* pokemon_x = FindGameEffects(0x0004000000055d00);
    const GameEffects* ocarina_us = FindGameEffects(0x0004000000033500);
    const GameEffects* ocarina_jp = FindGameEffects(0x0004000000033400);
    const GameEffects* majoras_mask_jp = FindGameEffects(0x00040000000d6e00);

    XSFLIB_CHECK(pokemon_x && std::string_view(pokemon_x->game) == "Pokémon X");
    XSFLIB_CHECK(pokemon_x && pokemon_x->buses[0].size() == 1 && pokemon_x->buses[1].empty() &&
                 pokemon_x->buses[0][0].kind == AuxEffect::Kind::kNwSndReverb &&
                 pokemon_x->buses[0][0].reverb.fused_time == kPokemonXFieldReverb.fused_time &&
                 pokemon_x->buses[0][0].reverb.filter_sizes == kPokemonXFieldReverb.filter_sizes);
    XSFLIB_CHECK(ocarina_us && ocarina_us == ocarina_jp);
    XSFLIB_CHECK(ocarina_us && ocarina_us->buses[1].size() == 1 && ocarina_us->buses[1][0].delay.delay_time == 96);
    XSFLIB_CHECK(majoras_mask_jp && majoras_mask_jp->buses[0].empty());
    for (const uint64_t id : {0x00040000001d1c00ull, 0x0004000000033600ull, 0x0004000000125500ull,
                              0x0004000000125600ull, 0x00040000000f8100ull, 0x000400000019bd00ull})
    {
        XSFLIB_CHECK(FindGameEffects(id) != nullptr);
    }
    XSFLIB_CHECK(FindGameEffects(0) == nullptr && FindGameEffects(UINT64_MAX) == nullptr);
}

// The table by SHA-1 finds its archives only. The archives of the table by program ID aren't in it.
void TestArchives()
{
    const ArchiveEffects* plaza = FindArchiveEffects(kMiiPlaza);
    const ArchiveEffects* luigis_mansion = FindArchiveEffects(kLuigisMansion);

    XSFLIB_CHECK(plaza && std::string_view(plaza->label) == "BGM_PEACE_MAIN" && plaza->buses[0].size() == 1 &&
                 plaza->buses[0][0].kind == AuxEffect::Kind::kNwSndReverb2011);
    XSFLIB_CHECK(luigis_mansion && luigis_mansion->buses[0].size() == 1 &&
                 luigis_mansion->buses[0][0].kind == AuxEffect::Kind::kFirmwareDelay &&
                 luigis_mansion->buses[0][0].delay.delay_time == 270);
    XSFLIB_CHECK(FindArchiveEffects(kPokemonX) == nullptr && FindArchiveEffects(kUnknown) == nullptr);
}

// Flower Town plays its SOUNDLIST_ sounds with its effect 0 where their start gives the archive's effects.
void TestLabelEffects()
{
    const ArchiveEffects* flower_town = FindArchiveEffects(kFlowerTown);

    XSFLIB_CHECK(flower_town && GameEffectOf(flower_town, "SOUNDLIST_TITLE", kArchiveEffects) == 0);
    XSFLIB_CHECK(GameEffectOf(flower_town, "BGM_FLOWER_TOWN", kArchiveEffects) == kArchiveEffects);
    XSFLIB_CHECK(GameEffectOf(flower_town, "SOUNDLIST_TITLE", 1) == 1);
    XSFLIB_CHECK(GameEffectOf(nullptr, "SOUNDLIST_TITLE", kArchiveEffects) == kArchiveEffects);
}

// An archive that holds the Plaza's label under another SHA-1 is another build of the Plaza's archive. The archive
// with the Plaza's SHA-1 isn't, and neither is one without a known label.
void TestOtherBuild()
{
    sar_files::ArchiveSpec spec;
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Sequence(ntdlib::sar::Flavor::kCtr, {0xFF}, {{"Start", 0}}), "", false}};
    spec.sounds = {{.name = "BGM_PEACE_MAIN", .file = 0}};
    const ntdlib::sar::SoundArchive known =
        ntdlib::sar::SoundArchive::Load(sar_files::Archive(ntdlib::sar::Flavor::kCtr, spec), {});
    spec.sounds = {{.name = "BGM_SOMETHING_ELSE", .file = 0}};
    const ntdlib::sar::SoundArchive unknown =
        ntdlib::sar::SoundArchive::Load(sar_files::Archive(ntdlib::sar::Flavor::kCtr, spec), {});

    const ArchiveEffects* other = FindOtherBuild(known, kUnknown);

    XSFLIB_CHECK(other && other == FindArchiveEffects(kMiiPlaza));
    XSFLIB_CHECK(FindOtherBuild(known, kMiiPlaza) == nullptr);
    XSFLIB_CHECK(FindOtherBuild(unknown, kUnknown) == nullptr);
}

// Whether each archive's game runs a reverb on bus A while a sound plays: the archives of both tables, the effects of
// some sequences alone, and a label effect.
void TestEffectsWhilePlaying()
{
    struct Case
    {
        std::string_view sha1;
        std::string_view label;
        uint8_t game_effect;
        bool reverb;
    };

    const Case kCases[] = {
        {kPokemonX, "", kArchiveEffects, true},
        {"6b471abb246fcaa2cf13ffbf16345e5d0c02e7e1", "", kArchiveEffects, true}, // WarioWare Gold
        {"c3fdb98e65e6143d76e6d971cfaaac058455300d", "", kArchiveEffects, true}, // Ocarina of Time 3D
        {kMajorasMask3D, "", kArchiveEffects, false},
        {kMiiPlaza, "BGM_PEACE_MAIN", kArchiveEffects, true},
        {"c2550ce04d4c8c44e92c2d72682038616fbec658", "", kArchiveEffects, true}, // the Plaza's system title
        {"a98dd3c22da8e4bed8327e4fc98196e102ce53ea", "", kArchiveEffects, true}, // Ultimate Angler
        {kFlowerTown, "BGM_FLOWER_TOWN", kArchiveEffects, true},
        {kFlowerTown, "SOUNDLIST_TITLE", kArchiveEffects, false},
        {"132e22e118ed9cb162fa2117d636d433920328a4", "", kArchiveEffects, true},  // Monster Manor
        {"c6a2a213c9780f9cbcd886d507ba72beb858b4a1", "", kArchiveEffects, true},  // Feed Mii
        {"ddb93158672258c7d8ea2b5edb8662bda34234e6", "", kArchiveEffects, true},  // Ninja Launcher
        {"7967baa712b4badc354a88e550847e1713195c74", "", kArchiveEffects, false}, // Warrior's Way, on bus B
        {"ff5231c919ad81331321e6e29e24d84bbf2127cc", "", kArchiveEffects, true},  // System Settings
        {"fd187b38953c162695779f26c66140859a63e74b", "", kArchiveEffects, true},  // the second System Settings
        {"0cc99a6732a7dbc0d6e8887449233ab25396672c", "", kArchiveEffects, true},  // Mii Maker
        {"ea55099f2b92479898b4eda7956d645021dac2d3", "", kArchiveEffects, true},  // the Activity Log
        {"32528650ee18385fdab7701dce1aae01bbd71e79", "", kArchiveEffects, true},  // Nintendo eShop
        {"cad032a7a698c87af8e73aa78e6865afe77a67fb", "", kArchiveEffects, true},  // Swapnote
        {kLuigisMansion, "", kArchiveEffects, false},
        {kNewLeaf, "SEQ_BGM_KAPPEI_GO_RANDOMMATCHING", kArchiveEffects, false},
        {kNewLeaf, "", 0, true},
        {kNewLeaf, "", 5, false},
        {kHappyHomeDesigner, "", kArchiveEffects, false},
        {kHappyHomeDesigner, "", 0, true},
        {kHappyHomeDesigner, "", 1, false},
        {"30c98d66881296eda5e676964918e7003b403c75", "", 0, true}, // Happy Home Designer's other release
    };

    for (const Case& c : kCases)
    {
        const AuxEffectLists* effects = EffectsWhilePlaying(c.sha1, c.label, c.game_effect);
        XSFLIB_CHECK(effects && ReverbOnBusA(*effects) == c.reverb);
    }
    XSFLIB_CHECK(EffectsWhilePlaying(kUnknown, "", kArchiveEffects) == nullptr);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ctr_game_effects",
                                    []
                                    {
                                        TestKinds();
                                        TestProgramIds();
                                        TestArchives();
                                        TestLabelEffects();
                                        TestOtherBuild();
                                        TestEffectsWhilePlaying();
                                    });
}
