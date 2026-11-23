// SPDX-License-Identifier: MIT

// What the Wii's and the Wii U's AXFX libraries share: the chorus's LFO table and interpolator taps, the range test
// of a gain, and the step, the clear and the release of a line. The two consoles' libraries hold the same tables. Both
// consoles' choruses read the tables here.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ntdlib::ax::axfx
{

// The chorus's LFO table: half a period of a sine in 128 steps, close to sin(pi * n / 128) * 2^24.
// __AXFXGetLfoSinTable returns its address. On the Wii U, the table is at 0x100036d4, and the function at 0x02011de0.
// On the Wii, the function is at 0x8010a9c0 (October 2006, 0x80034b14), and the table at 0x803aeba0 in Wii Sports,
// 0x80344600 in Wii Play and 0x805335d0 in Smooth Moves. The three games hold the same 128 words.
inline constexpr std::array<int32_t, 128> kChorusLfoSine = {
    0,        411733,   823219,   1234208,  1644454,  2053710,  2461728,  2868264,  3273072,  3675908,  4076531,
    4474698,  4870168,  5262706,  5652074,  6038037,  6420363,  6798821,  7173184,  7543226,  7908725,  8269460,
    8625213,  8975771,  9320922,  9660459,  9994176,  10321874, 10643354, 10958422, 11266890, 11568571, 11863283,
    12150850, 12431098, 12703857, 12968963, 13226258, 13475586, 13716797, 13949746, 14174291, 14390299, 14597637,
    14796184, 14985817, 15166424, 15337895, 15500126, 15653022, 15796488, 15930440, 16054795, 16169479, 16274424,
    16369565, 16454847, 16530216, 16595628, 16651044, 16696429, 16731757, 16757007, 16772163, 16777216, 16772163,
    16757007, 16731757, 16696429, 16651043, 16595628, 16530216, 16454846, 16369565, 16274424, 16169479, 16054794,
    15930439, 15796488, 15653022, 15500126, 15337894, 15166424, 14985817, 14796183, 14597638, 14390298, 14174290,
    13949744, 13716797, 13475586, 13226257, 12968964, 12703856, 12431096, 12150848, 11863283, 11568570, 11266888,
    10958422, 10643353, 10321872, 9994173,  9660458,  9320921,  8975768,  8625213,  8269458,  7908723,  7543227,
    7173183,  6798819,  6420360,  6038037,  5652073,  5262704,  4870169,  4474697,  4076529,  3675906,  3273072,
    2868263,  2461726,  2053710,  1644454,  1234206,  823215,   411733,
};

// The chorus's interpolator taps: 4 for each of 128 fractions of a sample. The libraries store them as floats. Each
// one is a multiple of 1/32,768. This table holds the multiples. __AXFXGetSrcCoef returns the address of a fraction's
// taps. On the Wii U, the table is at 0x10004150, and the function at 0x02018698. On the Wii, the function is at
// 0x8010a9cc (October 2006, 0x80034b20), and the table at 0x803aeda0 in Wii Sports, 0x80344800 in Wii Play and
// 0x805337d0 in Smooth Moves. The three games hold the same 2,048 bytes.
inline constexpr std::array<std::array<int16_t, 4>, 128> kChorusSrcMultiples = {{
    {3195, 26287, 3329, -32},   {3064, 26281, 3467, -34},   {2936, 26270, 3608, -38},   {2811, 26253, 3751, -42},
    {2688, 26230, 3897, -46},   {2568, 26202, 4046, -50},   {2451, 26169, 4199, -54},   {2338, 26130, 4354, -58},
    {2227, 26085, 4512, -63},   {2120, 26035, 4673, -67},   {2015, 25980, 4837, -72},   {1912, 25919, 5004, -76},
    {1813, 25852, 5174, -81},   {1716, 25780, 5347, -87},   {1622, 25704, 5522, -92},   {1531, 25621, 5701, -98},
    {1442, 25533, 5882, -103},  {1357, 25440, 6066, -109},  {1274, 25342, 6253, -115},  {1193, 25239, 6442, -121},
    {1115, 25131, 6635, -127},  {1040, 25018, 6830, -133},  {967, 24899, 7027, -140},   {897, 24776, 7227, -146},
    {829, 24648, 7430, -153},   {764, 24516, 7635, -159},   {701, 24379, 7842, -166},   {641, 24237, 8052, -174},
    {583, 24091, 8264, -181},   {526, 23940, 8478, -187},   {472, 23785, 8695, -194},   {420, 23626, 8914, -202},
    {371, 23462, 9135, -209},   {324, 23295, 9358, -215},   {279, 23123, 9583, -222},   {236, 22948, 9809, -230},
    {194, 22769, 10038, -237},  {154, 22586, 10269, -243},  {117, 22399, 10501, -250},  {81, 22208, 10735, -258},
    {47, 22015, 10970, -265},   {15, 21818, 11206, -271},   {-16, 21618, 11444, -277},  {-44, 21415, 11684, -283},
    {-71, 21208, 11924, -290},  {-97, 20999, 12166, -296},  {-121, 20786, 12409, -302}, {-143, 20571, 12653, -306},
    {-163, 20354, 12898, -311}, {-183, 20134, 13143, -316}, {-201, 19911, 13389, -321}, {-218, 19686, 13635, -325},
    {-234, 19459, 13882, -328}, {-248, 19230, 14130, -332}, {-261, 18998, 14377, -335}, {-273, 18765, 14625, -337},
    {-284, 18531, 14873, -339}, {-294, 18295, 15121, -341}, {-302, 18057, 15369, -341}, {-310, 17817, 15617, -341},
    {-317, 17577, 15864, -340}, {-323, 17335, 16111, -340}, {-328, 17092, 16357, -338}, {-332, 16848, 16603, -336},
    {-336, 16603, 16848, -332}, {-338, 16357, 17092, -328}, {-340, 16111, 17335, -323}, {-340, 15864, 17577, -317},
    {-341, 15617, 17817, -310}, {-341, 15369, 18057, -302}, {-341, 15121, 18295, -294}, {-339, 14873, 18531, -284},
    {-337, 14625, 18765, -273}, {-335, 14377, 18998, -261}, {-332, 14130, 19230, -248}, {-328, 13882, 19459, -234},
    {-325, 13635, 19686, -218}, {-321, 13389, 19911, -201}, {-316, 13143, 20134, -183}, {-311, 12898, 20354, -163},
    {-306, 12653, 20571, -143}, {-302, 12409, 20786, -121}, {-296, 12166, 20999, -97},  {-290, 11924, 21208, -71},
    {-283, 11684, 21415, -44},  {-277, 11444, 21618, -16},  {-271, 11206, 21818, 15},   {-265, 10970, 22015, 47},
    {-258, 10735, 22208, 81},   {-250, 10501, 22399, 117},  {-243, 10269, 22586, 154},  {-237, 10038, 22769, 194},
    {-230, 9809, 22948, 236},   {-222, 9583, 23123, 279},   {-215, 9358, 23295, 324},   {-209, 9135, 23462, 371},
    {-202, 8914, 23626, 420},   {-194, 8695, 23785, 472},   {-187, 8478, 23940, 526},   {-181, 8264, 24091, 583},
    {-174, 8052, 24237, 641},   {-166, 7842, 24379, 701},   {-159, 7635, 24516, 764},   {-153, 7430, 24648, 829},
    {-146, 7227, 24776, 897},   {-140, 7027, 24899, 967},   {-133, 6830, 25018, 1040},  {-127, 6635, 25131, 1115},
    {-121, 6442, 25239, 1193},  {-115, 6253, 25342, 1274},  {-109, 6066, 25440, 1357},  {-103, 5882, 25533, 1442},
    {-98, 5701, 25621, 1531},   {-92, 5522, 25704, 1622},   {-87, 5347, 25780, 1716},   {-81, 5174, 25852, 1813},
    {-76, 5004, 25919, 1912},   {-72, 4837, 25980, 2015},   {-67, 4673, 26035, 2120},   {-63, 4512, 26085, 2227},
    {-58, 4354, 26130, 2338},   {-54, 4199, 26169, 2451},   {-50, 4046, 26202, 2568},   {-46, 3897, 26230, 2688},
    {-42, 3751, 26253, 2811},   {-38, 3608, 26270, 2936},   {-34, 3467, 26281, 3064},   {-32, 3329, 26287, 3195},
}};

// The taps as the libraries' floats. Each multiple of 1/32,768 converts exactly.
inline constexpr std::array<std::array<float, 4>, 128> kChorusSrcTaps = []
{
    std::array<std::array<float, 4>, 128> taps{};
    for (std::size_t fraction = 0; fraction < taps.size(); fraction++)
    {
        for (std::size_t tap = 0; tap < 4; tap++)
        {
            taps[fraction][tap] = static_cast<float>(kChorusSrcMultiples[fraction][tap]) / 32768.0f;
        }
    }

    return taps;
}();

// The low 32 bits of a * b >> 24, with the product on 64 bits. The Wii U's chorus uses mullw and mulhwu with the
// corrections for signs. The Wii's uses mullw and mulhw, or mulhwu with corrections.
inline int32_t MultiplyShift24(int32_t a, int32_t b)
{
    return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 24);
}

// Whether a gain is below 0 or above 1. A NaN gain passes the libraries' tests.
inline bool OutOfRange(float value)
{
    return value < 0.0f || value > 1.0f;
}

// Moves a line's position on by a sample. The position wraps to 0 at the line's length.
inline void Step(uint32_t& position, uint32_t length)
{
    position++;
    if (position >= length)
    {
        position = 0;
    }
}

// Sets every sample of a line to 0.
template <typename Sample>
void Clear(std::vector<Sample>& line)
{
    std::fill(line.begin(), line.end(), Sample{});
}

// Releases the memory of each of `lines`, as the libraries free a line through their hooks.
template <typename Sample, std::size_t kCount>
void Free(std::array<std::vector<Sample>, kCount>& lines)
{
    for (std::vector<Sample>& line : lines)
    {
        line = std::vector<Sample>();
    }
}

} // namespace ntdlib::ax::axfx
