// SPDX-License-Identifier: MIT

// The model of JAudio2 (jaudio2): the load of a game's wave archives into the DSP's memory (LoadWaveSystems), on wave
// systems built for the test.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/jaudio2/engine.h"

using namespace xsflib::testing;

namespace
{

namespace nj = ntdlib::jaudio;
namespace j2 = ntdlib::jaudio2;

// Two systems. System 1 lists a.aw and b.aw, and system 2 lists b.aw again and c.aw, which the test doesn't give. Wave
// 3 of system 1 is at 8 in b.aw.
std::vector<nj::WaveSystem> Systems()
{
    nj::WaveSystem first{.number = 1};
    first.groups = {{.archive = "a.aw"}, {.archive = "b.aw"}};
    first.waves[3] = {.offset = 8, .archive = "b.aw"};
    nj::WaveSystem second{.number = 2};
    second.groups = {{.archive = "b.aw"}, {.archive = "c.aw"}};

    return {first, second};
}

// The archives go from the base on, each once, in the order of their names and on 32-byte boundaries. The default base
// is SoundFiles::kWaves. A wave's address in its bank follows its archive. A missing archive is named and gets no
// space.
void TestLoadWaveSystems()
{
    const std::map<std::string, std::vector<uint8_t>> files = {{"a.aw", std::vector<uint8_t>(40, 0xaa)},
                                                               {"b.aw", std::vector<uint8_t>(16, 0xbb)}};
    const j2::SoundFiles::WaveArchives waves = [&files](const std::string& name) -> const std::vector<uint8_t>*
    {
        const auto file = files.find(name);

        return file != files.end() ? &file->second : nullptr;
    };

    const j2::LoadedWaves at_base = j2::LoadWaveSystems(Systems(), waves, 0x8000);
    const j2::LoadedWaves at_default = j2::LoadWaveSystems(Systems(), waves);

    Check(at_base.memory->Size() == 0x8060 && at_base.memory->Byte(0x7fff) == 0 &&
              at_base.memory->Byte(0x8000) == 0xaa && at_base.memory->Byte(0x8027) == 0xaa &&
              at_base.memory->Byte(0x8028) == 0 && at_base.memory->Byte(0x8040) == 0xbb &&
              at_base.memory->Byte(0x804f) == 0xbb,
          "the archives from the base on");
    const j2::BankWave* wave = at_base.banks.at(1).Find(3);
    Check(wave != nullptr && wave->address == 0x8048, "a wave's address follows its archive");
    Check(at_base.missing == std::vector<std::string>{"c.aw"}, "the missing archive");
    const j2::BankWave* default_wave = at_default.banks.at(1).Find(3);
    Check(at_default.memory->Size() == j2::SoundFiles::kWaves + 0x60 &&
              at_default.memory->Byte(j2::SoundFiles::kWaves) == 0xaa && default_wave != nullptr &&
              default_wave->address == j2::SoundFiles::kWaves + 0x48,
          "the default base");
}

} // namespace

int main()
{
    return RunTest("jaudio2", TestLoadWaveSystems);
}
