// SPDX-License-Identifier: MIT

// Tests of AX's shared parts (ntdlib/ax): the voice allocation against a model of AX's voice lists, and the low-pass
// coefficients against reference functions for every 16-bit frequency and against filter_tables.py's poles.

#include "ntdlib/ax/ax.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ax/lpf.h"
#include "ntdlib/ax/voices.h"

namespace ax = ntdlib::ax;
using xsflib::testing::Check;

namespace
{

// The lists as vectors: the free stack with its top at the back, and each priority's voices from the oldest.
struct Model
{
    explicit Model(int count)
    {
        for (uint32_t v = 0; v < static_cast<uint32_t>(count); v++)
        {
            free.push_back(v);
        }
    }

    std::optional<ax::Voices::Taken> Take(uint32_t priority)
    {
        if (!free.empty())
        {
            const uint32_t v = free.back();
            free.pop_back();
            return ax::Voices::Taken{v, false};
        }

        for (uint32_t lower = ax::kPriorityMin; lower < priority; lower++)
        {
            if (!lists[lower].empty())
            {
                const uint32_t v = lists[lower].front();
                lists[lower].erase(lists[lower].begin());
                return ax::Voices::Taken{v, true};
            }
        }

        return std::nullopt;
    }

    void Push(uint32_t v, uint32_t priority)
    {
        if (priority == ax::kPriorityFree)
        {
            free.push_back(v);
        }
        else
        {
            lists[priority].push_back(v);
        }
    }

    void Remove(uint32_t v)
    {
        free.erase(std::remove(free.begin(), free.end(), v), free.end());

        for (auto& list : lists)
        {
            list.erase(std::remove(list.begin(), list.end(), v), list.end());
        }
    }

    std::vector<uint32_t> free;
    std::vector<std::vector<uint32_t>> lists = std::vector<std::vector<uint32_t>>(ax::kPriorityMax + 1);
};

// Whether `voices` holds the model's lists, in the model's order both ways.
bool Same(const ax::Voices& voices, const Model& model)
{
    std::vector<uint32_t> top_first;
    voices.ForEachNewestFirst(ax::kPriorityFree, [&](uint32_t v) { top_first.push_back(v); });
    if (!std::equal(top_first.begin(), top_first.end(), model.free.rbegin(), model.free.rend()))
    {
        return false;
    }

    for (uint32_t p = ax::kPriorityMin; p <= ax::kPriorityMax; p++)
    {
        std::vector<uint32_t> oldest, newest;
        voices.ForEachOldestFirst(p, [&](uint32_t v) { oldest.push_back(v); });
        voices.ForEachNewestFirst(p, [&](uint32_t v) { newest.push_back(v); });
        if (oldest != model.lists[p] || !std::equal(newest.begin(), newest.end(), oldest.rbegin(), oldest.rend()))
        {
            return false;
        }

        for (const uint32_t v : oldest)
        {
            if (voices.Priority(v) != p)
            {
                return false;
            }
        }
    }

    return true;
}

void TestVoices()
{
    ax::Voices voices;

    const auto first = voices.Take(1);

    Check(first && first->voice == 95 && !first->stolen, "the first voice taken is the last");
    Check(ax::Voices(64).Take(1)->voice == 63, "with 64 voices, the first taken is 63");

    // Every voice at priority 5: a request at 5 finds none, and one at 6 takes the oldest voice of priority 5.
    ax::Voices full;
    std::vector<uint32_t> order;
    for (int i = 0; i < ax::kVoiceCount; i++)
    {
        const uint32_t v = full.Take(5)->voice;
        full.Push(v, 5);
        order.push_back(v);
    }

    Check(!full.Take(5) && !full.Take(1), "no voice below the request");

    const auto stolen = full.Take(6);

    Check(stolen && stolen->voice == order.front() && stolen->stolen, "a steal takes the oldest voice");

    // The lowest priority goes first.
    ax::Voices mixed(2);
    mixed.Push(mixed.Take(3)->voice, 3);
    mixed.Push(mixed.Take(2)->voice, 2);

    Check(mixed.Take(10)->voice == 0, "a steal takes from the lowest priority");

    // A voice that's in no list stays out, and the lists stay whole.
    ax::Voices twice(3);
    for (int i = 0; i < 3; i++)
    {
        twice.Push(twice.Take(4)->voice, 4);
    }

    twice.Remove(1);
    twice.Remove(1);

    std::vector<uint32_t> left;
    twice.ForEachOldestFirst(4, [&](uint32_t v) { left.push_back(v); });
    Check(left == std::vector<uint32_t>({2, 0}), "removing a voice twice");

    // A copy is a snapshot.
    const ax::Voices copy = twice;

    twice.Push(1, 9);

    std::vector<uint32_t> in_copy;
    copy.ForEachOldestFirst(9, [&](uint32_t v) { in_copy.push_back(v); });
    Check(in_copy.empty(), "a copy keeps its lists");

    // Random acquires, frees and priority changes, against the model.
    std::mt19937 random(20261011);
    for (const int count : {96, 64, 5})
    {
        ax::Voices tested(count);
        Model model(count);
        bool same = true;
        for (int step = 0; step < 20000 && same; step++)
        {
            const uint32_t v = random() % static_cast<uint32_t>(count);
            const uint32_t priority = random() % (ax::kPriorityMax + 1);
            switch (random() % 4)
            {
            case 0:
            case 1: // AXAcquireVoice
                {
                    const auto a = tested.Take(priority);
                    const auto b = model.Take(priority);
                    same = a.has_value() == b.has_value() && (!a || (a->voice == b->voice && a->stolen == b->stolen));

                    if (a)
                    {
                        tested.Push(a->voice, priority);
                        model.Push(b->voice, priority);
                    }
                    break;
                }

            case 2: // AXFreeVoice
                if (tested.Priority(v) != ax::kPriorityFree)
                {
                    tested.Remove(v);
                    model.Remove(v);
                    tested.Push(v, ax::kPriorityFree);
                    model.Push(v, ax::kPriorityFree);
                }
                break;

            default: // AXSetVoicePriority
                if (tested.Priority(v) != ax::kPriorityFree && priority != ax::kPriorityFree)
                {
                    tested.Remove(v);
                    model.Remove(v);
                    tested.Push(v, priority);
                    model.Push(v, priority);
                }
                break;
            }

            same = same && Same(tested, model);
        }

        Check(same, "random operations keep the model's lists");
    }
}

// AXGetLpfCoefs (Wii Sports' main.dol 0x801074dc).
std::pair<uint16_t, uint16_t> WiiLpf(uint16_t freq)
{
    const float angle = 6.2831855f * static_cast<float>(freq) / static_cast<float>(32000);
    const float c = 2.0f - static_cast<float>(std::cos(static_cast<double>(angle)));
    const float root = static_cast<float>(std::sqrt(static_cast<double>(c * c - 1.0f)));
    const auto b0 = static_cast<uint16_t>(static_cast<int32_t>(32768.0f * -(root - c)));

    return {static_cast<uint16_t>(0x7fff - b0), b0};
}

// AXComputeLpfCoefs for snd_core (32 kHz) and sndcore2 (the renderer's rate, and a fused step).
std::pair<uint16_t, uint16_t> WiiULpf(uint32_t frequency, bool sndcore2, int samples_per_frame)
{
    const float rate = sndcore2 ? static_cast<float>(samples_per_frame * 1000 / 3) : 32000.0f;
    const float w = 6.2831855f * static_cast<float>(frequency) / rate;
    const float k = 2.0f - std::cos(w);
    const float k2 = sndcore2 ? std::fma(k, k, -1.0f) : k * k - 1.0f;
    const float x = (k - std::sqrt(k2)) * 32768.0f;
    const int32_t b0 = !(x >= -2147483648.0f && x < 2147483648.0f) ? INT32_MIN : static_cast<int32_t>(x);

    return {static_cast<uint16_t>(32767 - static_cast<uint16_t>(b0)), static_cast<uint16_t>(b0)};
}

// On the Mac, float and double cos and sqrt give the same coefficients for every frequency. The settings keep each
// console's arithmetic for the platforms where a float cos differs.
void TestLpf()
{
    int wii = 0, snd_core = 0, sndcore2_32 = 0, sndcore2_48 = 0;
    for (uint32_t f = 0; f <= 0xffff; f++)
    {
        const auto same = [](ax::LpfCoefs c, std::pair<uint16_t, uint16_t> r)
        {
            return c.a0 == r.first && c.b0 == r.second;
        };
        wii += !same(ax::ComputeLpfCoefs(f, {.double_precision = true}), WiiLpf(static_cast<uint16_t>(f)));
        snd_core += !same(ax::ComputeLpfCoefs(f, {}), WiiULpf(f, false, 96));
        sndcore2_32 += !same(ax::ComputeLpfCoefs(f, {.fused = true}), WiiULpf(f, true, 96));
        sndcore2_48 += !same(ax::ComputeLpfCoefs(f, {.rate = 48000.0f, .fused = true}), WiiULpf(f, true, 144));
    }

    Check(wii == 0, "the Wii's coefficients for every frequency");
    Check(snd_core == 0, "snd_core's coefficients for every frequency");
    Check(sndcore2_32 == 0 && sndcore2_48 == 0, "sndcore2's coefficients at 32 and 48 kHz for every frequency");
    Check(ax::ComputeLpfCoefs(0, {}).b0 == 32768 && ax::ComputeLpfCoefs(0, {}).a0 == 0xffff,
          "a cutoff of 0 gives b0 = 32768, as on both consoles");
}

// A cutoff in Hz and AX's pole b0 for it.
struct Pole
{
    uint32_t hz;
    uint16_t b0;
};

// tools/sequence_export/filter_tables.py fits the Wii U's and the Wii's SoundFont and SFZ low-pass filters to these
// poles. The script computes them in Python with AX's arithmetic. Its --poles option prints them.
constexpr Pole kCafePoles[23] = {
    {80, 32257},   {100, 32130},  {128, 31954},  {160, 31754},  {200, 31506},  {256, 31161},
    {320, 30773},  {400, 30294},  {500, 29706},  {640, 28903},  {800, 28013},  {1000, 26943},
    {1280, 25519}, {1600, 23995}, {2000, 22235}, {2560, 20026}, {3200, 17829}, {4000, 15512},
    {5120, 12933}, {6400, 10727}, {8000, 8780},  {10240, 7068}, {12800, 6030},
};

constexpr Pole kRevolutionPoles[58] = {
    {31, 32568},   {34, 32549},   {38, 32524},   {43, 32492},   {48, 32460},   {53, 32428},   {59, 32390},
    {66, 32346},   {74, 32295},   {82, 32244},   {92, 32181},   {102, 32118},  {114, 32042},  {127, 31961},
    {142, 31867},  {158, 31767},  {176, 31655},  {196, 31531},  {219, 31389},  {244, 31235},  {272, 31064},
    {303, 30875},  {338, 30664},  {377, 30431},  {420, 30175},  {468, 29893},  {522, 29578},  {581, 29238},
    {648, 28858},  {722, 28443},  {805, 27986},  {897, 27488},  {1000, 26943}, {1114, 26353}, {1241, 25712},
    {1383, 25016}, {1542, 24263}, {1718, 23459}, {1915, 22596}, {2134, 21681}, {2378, 20713}, {2650, 19697},
    {2953, 18638}, {3291, 17543}, {3668, 16420}, {4087, 15286}, {4555, 14146}, {5076, 13021}, {5656, 11925},
    {6303, 10871}, {7025, 9875},  {7828, 8955},  {8724, 8119},  {9721, 7384},  {10834, 6756}, {12073, 6249},
    {13454, 5876}, {14993, 5661},
};

// The script and ComputeLpfCoefs compute the same poles in two languages. This check keeps them the same.
void TestLpfAgainstFilterTables()
{
    int cafe = 0;
    for (const Pole& pole : kCafePoles)
    {
        cafe += ax::ComputeLpfCoefs(pole.hz, {.fused = true}).b0 != pole.b0;
    }

    int revolution = 0;
    for (const Pole& pole : kRevolutionPoles)
    {
        revolution += ax::ComputeLpfCoefs(pole.hz, {.double_precision = true}).b0 != pole.b0;
    }

    Check(cafe == 0, "ComputeLpfCoefs gives filter_tables.py's poles for the Wii U");
    Check(revolution == 0, "ComputeLpfCoefs gives filter_tables.py's poles for the Wii");
}

} // namespace

int main()
{
    TestVoices();
    TestLpf();
    TestLpfAgainstFilterTables();

    return xsflib::testing::Summary("ax");
}
