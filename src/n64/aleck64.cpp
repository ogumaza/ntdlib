// SPDX-License-Identifier: MIT

#include "ntdlib/n64/aleck64.h"

#include <array>
#include <span>
#include <string_view>

namespace ntdlib::n64
{

namespace
{

using xsflib::rip::RomChip;

const std::array kElevenBeatChips = {RomChip{"824911452639cedf6a8186c05cd046e61fc98896", 0x800000, 0, true}};
const std::array kHiPaiParadiseChips = {
    RomChip{"e909ea5b71b81087da07821c4f57244576363678", 0x1000000, 0, true},
    RomChip{"7c3e59f6520dc3f0aa592e682fa82e30ffd1f4d0", 0x1000000, 0x1000000, true}};
const std::array kHiPaiParadise2Chips = {
    RomChip{"279a006436069232383b2065e7416d8ccc76e3ca", 0x1000000, 0, true},
    RomChip{"a6d2d805f96f0ad7294ebf41b13573320154b2a4", 0x1000000, 0x1000000, true}};
const std::array kTowerAndShaftChips = {RomChip{"7dfa3447d2c489c0448c4004dc12d3037c05a0f3", 0x1000000, 0, true}};
const std::array kHanabiDeDoonChips = {RomChip{"4f41ee23edc18110be1218ba333d1c58376ab175", 0x1000000, 0, true},
                                       RomChip{"c8cc6c0a456b593aef711d0a75b2342ba2f8203f", 0x1000000, 0x1000000, true}};
const std::array kKurukuruFeverChips = {
    RomChip{"824fdce01fffdfcbcc9b1fbda4ab389a10b2b418", 0x1000000, 0, true},
    RomChip{"2b6ca1a769dee74e112c2b287dacd0bf46dda091", 0x1000000, 0x1000000, true}};
const std::array kMagicalTetrisChallengeChips = {
    RomChip{"59932c70b43ff8e9264c670f37b3abbe939b7f95", 0x1000000, 0, true}};
const std::array kMayjinsen3Chips = {RomChip{"b5834bfde5b8a7e20415b2593abd76ec95ab27c7", 0x800000, 0, true}};
const std::array kStarSoldierChips = {RomChip{"3794606c008fb69f5d16dcccece94d03da23bf8a", 0xC00000, 0, true}};
const std::array kSuperRealMahjongVsChips = {RomChip{"a78de955f2fcd99dda14e782984368b320eb5415", 0x2000000, 0, true}};
const std::array kVividDollsChips = {RomChip{"06d8b808cc19378b046803f4dc75c7d791b7767f", 0x800000, 0, true}};

// A set: the SHA-1 of the image that its chips make, and the chips.
struct Aleck64Set
{
    std::string_view image_sha1;
    std::span<const RomChip> chips;
};

const std::array kSets = {
    Aleck64Set{"5432056f34c378b5862009e5b79a256c10c5a20e", kElevenBeatChips},
    Aleck64Set{"da401fc96a2b69db81970d879a2255e54db88e34", kMayjinsen3Chips},
    Aleck64Set{"a29348632ed7726d75d693dfb007f277c1f6a563", kStarSoldierChips},
    Aleck64Set{"a590216a171a61517cd07ff629d8215631c8be87", kSuperRealMahjongVsChips},
    Aleck64Set{"747e9420cce23ff82b5922cd91ec43272d03fa4f", kVividDollsChips},
    Aleck64Set{"68329c94e1b9342ee60713e9e0945ea2663210c7", kHiPaiParadiseChips},
    Aleck64Set{"ba4474dc4967adb2a41cf3b0022c6215990c0e64", kTowerAndShaftChips},
    Aleck64Set{"2a77e47d34b8d9d89eb37ffc062e9ec0c2121abc", kHanabiDeDoonChips},
    Aleck64Set{"47fdc89620ddf68972e861dee1536a26a309e35c", kHiPaiParadise2Chips},
    Aleck64Set{"f59c121e4d4c2c57c2ea4728919775a8d5d5e02e", kKurukuruFeverChips},
    Aleck64Set{"7e357995def4078a54ba8e4486f361d631ff3329", kMagicalTetrisChallengeChips},
};

} // namespace

std::span<const RomChip> Aleck64Chips(std::string_view image_sha1)
{
    for (const Aleck64Set& set : kSets)
    {
        if (set.image_sha1 == image_sha1)
        {
            return set.chips;
        }
    }

    return {};
}

} // namespace ntdlib::n64
