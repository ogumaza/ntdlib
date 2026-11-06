// SPDX-License-Identifier: MIT

// Checks the models of Nintendo Land's effect classes (cafe/snd/nintendo_land_effects.h) against the classes' code.
// Each setting runs a test signal through both of a class's paths. The SHA-256 of each output was recorded from the
// game's code (Lunch.rpx of the American release) run under Unicorn, a CPU emulator, from a hand-set state. The
// settings are those of the game's scenes, and a delay with feedback.

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/sha256.h"
#include "xsflib/testing/check.h"

#include "ntdlib/cafe/axfx/axfx.h"
#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/cafe/snd/nintendo_land_effects.h"

namespace
{

using ntdlib::cafe::snd::EffectParamWords;
using ntdlib::cafe::snd::EffectType;
using ntdlib::cafe::snd::nintendo_land::Delay;
using ntdlib::cafe::snd::nintendo_land::kFrameSamples;
using ntdlib::cafe::snd::nintendo_land::Reverb;

constexpr std::size_t kFrames = 80;
constexpr std::size_t kSamples = kFrames * kFrameSamples;

// The test signal, for the left and right channels: an impulse on each channel, a stretch of noise from a linear
// congruential generator, and the extremes of a 32-bit sample.
std::array<std::vector<int32_t>, 2> Signal()
{
    std::array<std::vector<int32_t>, 2> signal{std::vector<int32_t>(kSamples), std::vector<int32_t>(kSamples)};
    signal[0][0] = 1 << 23;
    signal[1][10] = -(1 << 23);
    uint32_t state = 711;
    for (std::size_t i = 1000; i < 2000; i++)
    {
        for (std::vector<int32_t>& channel : signal)
        {
            state = state * 1664525u + 1013904223u;
            channel[i] = static_cast<int32_t>(state >> 11) - (1 << 20);
        }
    }
    signal[0][3000] = INT32_MAX;
    signal[1][3000] = INT32_MIN;

    return signal;
}

// Runs the signal through `effect` a frame at a time, and gives the SHA-256 of the output: 32-bit little-endian
// samples, left then right in each pair. The surround channel has to stay as it is.
template <typename Effect>
std::string OutputSha256(Effect& effect)
{
    std::array<std::vector<int32_t>, 2> signal = Signal();
    std::vector<int32_t> surround(kFrameSamples, 5);
    for (std::size_t frame = 0; frame < kFrames; frame++)
    {
        const std::size_t first = frame * kFrameSamples;
        effect.Process({&signal[0][first], &signal[1][first], surround.data()}, kFrameSamples);
    }
    XSFLIB_CHECK(surround == std::vector<int32_t>(kFrameSamples, 5));

    std::vector<uint8_t> bytes(8 * kSamples);
    for (std::size_t i = 0; i < kSamples; i++)
    {
        xsflib::StoreLe32(&bytes[8 * i], static_cast<uint32_t>(signal[0][i]));
        xsflib::StoreLe32(&bytes[8 * i + 4], static_cast<uint32_t>(signal[1][i]));
    }

    return xsflib::Sha256::Hex(xsflib::Sha256::Of(bytes));
}

struct Setting
{
    const char* name;
    EffectType type;
    std::vector<uint32_t> words; // with the threaded path's flag set
    const char* unthreaded;      // the output's SHA-256 through the unthreaded path
    const char* threaded;        // and through the threaded path
};

// The scenes' reverbs (from the calls to the game's set-up at 0x0201F918 and 0x028D4D2C) and delays. DBT, FZR, GWR,
// LIG, MTR, PKM and YSI are the attractions' scenes (NZM and ZLD set LIG's reverb). TTL is the plaza. Its delay runs
// before its reverb. The echo is a delay with feedback that no scene sets.
const std::vector<Setting>& Settings()
{
    static const std::vector<Setting> settings{
        {"dbt",
         EffectType::kNintendoLandReverb,
         {0x3d4ccccd, 0x3f000000, 0x3f19999a, 0x3ecccccd, 0x3f800000, 2, 2, 0x3e99999a, 0x3e99999a, 0, 0x01000000, 8,
          4},
         "cee240e0a3068a715970b37b1cdd9f27239e1d1503303a4d3b4b9c245c05c499",
         "6d569ea87bcf4f614a0f4c440ace27446266d71363bda8ca26b88cd57468c7b4"},
        {"fzr",
         EffectType::kNintendoLandReverb,
         {0x3ca3d70a, 0x3f99999a, 0x3f19999a, 0x3ecccccd, 0x3f800000, 3, 3, 0x3e4ccccd, 0x3f59999a, 0, 0x01000000, 8,
          4},
         "b354912a753d604617321b17fd765195aff9980cc576792862d2dcd37fe28433",
         "238cc59fea985c15e6ce198eced8d5c96fd54cbe5d122a9a4bdcc0936335621f"},
        {"gwr",
         EffectType::kNintendoLandReverb,
         {0x3ca3d70a, 0x3f800000, 0x3f19999a, 0x3ecccccd, 0x3f800000, 1, 3, 0x3dcccccd, 0x3f666666, 0, 0x01000000, 8,
          4},
         "0b8c39f35e97c7aad6b2445c5ec1c07fd291a1fd0339e28afd1049885af85764",
         "ee4577ff624bb88ef2912807fe6c7398a5c32b5e13680abf10b701b7d885ca71"},
        {"lig",
         EffectType::kNintendoLandReverb,
         {0x3ca3d70a, 0x3f800000, 0x3f19999a, 0x3ecccccd, 0x3f800000, 3, 3, 0x3dcccccd, 0x3f666666, 0, 0x01000000, 8,
          4},
         "21a9ffe9566a88d333b5840c5635f74af61f88bd2d0bf27c6f6182396f3be5c6",
         "e66fe20da987df26124bfa019a9f16c5e22bc66874ee7a6a4e7ee7c0054fb01c"},
        {"mtr",
         EffectType::kNintendoLandReverb,
         {0x3ca3d70a, 0x3f800000, 0x3f19999a, 0x3ecccccd, 0x3f800000, 5, 4, 0x3dcccccd, 0x3f666666, 0, 0x01000000, 8,
          4},
         "8e66ecc1e95a46d1040bb159ff76669cf7a94e788e876c39a98b01027fdbd796",
         "d3f763ffe4075f3fe70a0856bdea049c30b362bc043daf10cf78dd45b93eecb4"},
        {"pkm",
         EffectType::kNintendoLandReverb,
         {0x3ca3d70a, 0x3f800000, 0x3f19999a, 0x3ecccccd, 0x3f800000, 3, 5, 0x3dcccccd, 0x3f666666, 0, 0x01000000, 8,
          4},
         "bc7c659881b2b804b72309252bec2434a2a281e8c0e7b5e60e2093e44054d62f",
         "e079d1282ba9a74ab32c4bbc7c5993ddfa9c7c3d8e7a3b285d7049a122b08f1c"},
        {"ysi",
         EffectType::kNintendoLandReverb,
         {0x3ca3d70a, 0x3f800000, 0x3f19999a, 0x3ecccccd, 0x3f800000, 0, 2, 0x3dcccccd, 0x3f666666, 0, 0x01000000, 8,
          4},
         "c6dbb7e6ef06f45cf090d1b9bf2c177220b3d8a2863ff3bf7638528c68d84599",
         "892b3e87e762ced1ca846c51e5da9159aaba7579125d1a41d87e08ee9a2a290b"},
        {"ttl",
         EffectType::kNintendoLandReverb,
         {0, 0x40400000, 0x3f19999a, 0x3ecccccd, 0x3f800000, 0, 0, 0, 0x3f800000, 1, 0x01000000, 8, 4},
         "bebf6bfbcb309083ce2967b14f2a8ef317404d53843468ee49614e1560d90c15",
         "3532f28f7130a32ac82caaac712dd3b69138a4b2d7c8107acf02bc3e57caa884"},
        {"ttl_delay",
         EffectType::kNintendoLandDelay,
         {0x3d23d70a, 0, 0x3f000000, 0x3e99999a, 1, 0x01000000, 8, 4},
         "5ad60f8f7d094545260846204036d1e3c91c3995f46ad6f5e179447b61487ed5",
         "58528f4a9ff36d949a81eb989cd2be6df6c540afd26f314960acb10c15288104"},
        {"echo",
         EffectType::kNintendoLandDelay,
         {0x41200000, 0x3f000000, 0x3f333333, 0x3ca3d70a, 0, 0x01000000, 8, 4},
         "bd95c82219d67de0ed4cac5bf02551a4e88f319b6a268be7afcab5d9a930d726",
         "d6ceb4103d77afd60c60419273387c4417df0014e03e75cc6122fe8d3a3f7002"},
    };

    return settings;
}

template <typename Effect>
void CheckSetting(const Setting& setting)
{
    XSFLIB_CHECK(setting.words.size() == EffectParamWords(setting.type) && Effect::kParamWords == setting.words.size());

    Effect threaded;
    XSFLIB_CHECK(threaded.Init(setting.words));
    const std::string threaded_hash = OutputSha256(threaded);

    std::vector<uint32_t> words = setting.words;
    words[Effect::kParamWords - 3] = 0;
    Effect unthreaded;
    XSFLIB_CHECK(unthreaded.Init(words));
    const std::string unthreaded_hash = OutputSha256(unthreaded);

    if (!xsflib::testing::Check(threaded_hash == setting.threaded && unthreaded_hash == setting.unthreaded,
                                setting.name))
    {
        *seqlib::testing::failure_stream << "  threaded " << threaded_hash << "\n  unthreaded " << unthreaded_hash
                                         << '\n';
    }
}

void TestSettings()
{
    for (const Setting& setting : Settings())
    {
        if (setting.type == EffectType::kNintendoLandReverb)
        {
            CheckSetting<Reverb>(setting);
        }
        else
        {
            CheckSetting<Delay>(setting);
        }
    }
}

// Init refuses an index past the end of its table, a ring of no frames, a line longer than kMaxLength and too few
// words. A refused effect leaves its frames as they are.
void TestRefusals()
{
    const std::vector<uint32_t> reverb = Settings()[3].words;
    const std::vector<uint32_t> delay = Settings()[9].words;

    Reverb r;
    std::vector<uint32_t> words = reverb;
    words[5] = 8;
    XSFLIB_CHECK(!r.Init(words));
    words = reverb;
    words[6] = 6;
    XSFLIB_CHECK(!r.Init(words));
    words = reverb;
    words[11] = 0;
    XSFLIB_CHECK(!r.Init(words));
    words[10] = 0;
    XSFLIB_CHECK(r.Init(words));
    words = reverb;
    words[0] = 0x42c80000; // 100 s
    XSFLIB_CHECK(!r.Init(words));
    XSFLIB_CHECK(!r.Init(std::span(reverb).first(Reverb::kParamWords - 1)));

    std::array<int32_t, kFrameSamples> left{};
    std::array<int32_t, kFrameSamples> right{};
    left[0] = 1000;
    r.Process({left.data(), right.data(), nullptr}, kFrameSamples);
    XSFLIB_CHECK(left[0] == 1000);

    Delay d;
    words = delay;
    words[6] = 0;
    XSFLIB_CHECK(!d.Init(words));
    words = delay;
    words[0] = 0x4b000000; // 2^23 ms
    XSFLIB_CHECK(!d.Init(words));
    XSFLIB_CHECK(!d.Init(std::span(delay).first(Delay::kParamWords - 1)));
    d.Process({left.data(), right.data(), nullptr}, kFrameSamples);
    XSFLIB_CHECK(left[0] == 1000);

    // A frame of fewer than 96 samples stays as it is.
    XSFLIB_CHECK(d.Init(delay));
    d.Process({left.data(), right.data(), nullptr}, kFrameSamples - 1);
    XSFLIB_CHECK(left[0] == 1000);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("cafe_nintendo_land_effects",
                                    []
                                    {
                                        TestSettings();
                                        TestRefusals();
                                    });
}
