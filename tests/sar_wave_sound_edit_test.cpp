// SPDX-License-Identifier: MIT

// Tests of the edits of wave sound files (ntdlib/sar/wave_sound_edit.h). sar_files.h builds the files in both byte
// orders. The reader of formats.h reads each edit back. Every byte that an edit leaves alone has to stay.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_sound_edit.h"
#include "ntdlib/testing/sar_files.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace sar_files = ntdlib::testing::sar_files;
using ntdlib::sar::Flavor;
using ntdlib::sar::WaveId;
using ntdlib::sar::WaveSoundFile;
using ntdlib::sar::WaveSoundPlaces;
using ntdlib::sar::WaveSoundValuePlaces;
using ntdlib::sar::WithWaveSoundValues;
using ntdlib::sar::WithWaveSoundWave;

template <typename Error, typename Call>
bool Throws(Call call)
{
    return !ErrorOf<Error>(call).empty();
}

WaveSoundFile Read(std::span<const uint8_t> file, Flavor flavor)
{
    ntdlib::sar::Options options;
    options.flavor = flavor;
    options.rules = ntdlib::sar::Rules::kStopAtFirstProblem;

    return WaveSoundFile::Parse(file, options);
}

// A wave sound that plays the wave of entry `wave`, with a note event.
sar_files::WaveSoundSpec Sound(uint32_t wave)
{
    sar_files::WaveSoundSpec sound;
    sound.tracks = {{{0.0f, 1.0f, 0}}};
    sound.notes.resize(1);
    sound.notes[0].wave = wave;

    return sound;
}

// A wave sound with every value, one without any, and a third that shares the first one's wave. The table lists the
// waves as the tools list them: in the order in which the notes first name them.
sar_files::WaveSoundFileSpec Spec()
{
    sar_files::WaveSoundSpec full = Sound(0);
    full.pan = 20;
    full.span = 10;
    full.pitch = 1.25f;
    full.filter = 0x00030240;
    full.sends = {100, 3, 10, 20, 30};
    full.adshr = std::array<uint8_t, 5>{127, 100, 90, 80, 70};

    sar_files::WaveSoundFileSpec spec;
    spec.waves = {{0x05000001, 3}, {0x05000002, 0}};
    spec.sounds = {full, Sound(1), Sound(0)};

    return spec;
}

// The waves that the reader gives the sounds' first notes.
std::vector<WaveId> NoteWaves(const WaveSoundFile& file)
{
    std::vector<WaveId> waves;
    for (const WaveSoundFile::WaveSound& sound : file.sounds)
    {
        waves.push_back(file.waves[sound.notes[0].wave_id_index]);
    }

    return waves;
}

void TestPlaces(Flavor flavor)
{
    const std::vector<uint8_t> file = sar_files::WaveSoundFile(flavor, Spec());

    const WaveSoundPlaces full = WaveSoundValuePlaces(file, flavor, 0);
    const WaveSoundPlaces bare = WaveSoundValuePlaces(file, flavor, 1);

    XSFLIB_CHECK((full == WaveSoundPlaces{true, true, true, true, 3, true}));
    XSFLIB_CHECK(bare == WaveSoundPlaces());

    // The libraries read the filters only from version 1.1.0 on.
    std::vector<uint8_t> old = file;
    old[flavor == Flavor::kCafe ? 10 : 9] = 0;
    XSFLIB_CHECK(!WaveSoundValuePlaces(old, flavor, 0).filters && WaveSoundValuePlaces(old, flavor, 0).pitch);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { WaveSoundValuePlaces(file, flavor, 3); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { WaveSoundValuePlaces(file, Flavor::kCtr, 0); }));
}

// New values read back. The file keeps its size, and only the bytes of the values change.
void TestValues(Flavor flavor)
{
    // The pan's value holds two more bytes. The edit keeps them.
    std::vector<uint8_t> file = sar_files::WaveSoundFile(flavor, Spec());
    const std::array<uint8_t, 4> pan_value =
        flavor == Flavor::kCafe ? std::array<uint8_t, 4>{0, 0, 10, 20} : std::array<uint8_t, 4>{20, 10, 0, 0};
    const auto pan_at = std::search(file.begin(), file.end(), pan_value.begin(), pan_value.end());
    XSFLIB_CHECK(pan_at != file.end());
    pan_at[flavor == Flavor::kCafe ? 0 : 2] = 0xAB;
    WaveSoundFile::WaveSound values = Read(file, flavor).sounds[0];
    values.pan = 100;
    values.span = 30;
    values.pitch = 0.5f;
    values.low_pass = 10;
    values.biquad_type = 4;
    values.biquad_value = 50;
    values.main_send = 90;
    values.aux_sends = {1, 2, 3};
    values.adshr = {10, 20, 30, 40, 50};

    const std::vector<uint8_t> edited = WithWaveSoundValues(file, flavor, 0, values);

    const WaveSoundFile read = Read(edited, flavor);
    const WaveSoundFile::WaveSound& sound = read.sounds[0];
    XSFLIB_CHECK(sound.pan == 100 && sound.span == 30 && sound.pitch == 0.5f);
    XSFLIB_CHECK(sound.low_pass == 10 && sound.biquad_type == 4 && sound.biquad_value == 50);
    XSFLIB_CHECK(sound.main_send == 90 && sound.aux_sends == (std::array<uint8_t, 3>{1, 2, 3}));
    XSFLIB_CHECK((sound.adshr == ntdlib::sar::Adshr{10, 20, 30, 40, 50}));
    XSFLIB_CHECK(edited.size() == file.size());
    XSFLIB_CHECK(edited[static_cast<std::size_t>(pan_at - file.begin()) + (flavor == Flavor::kCafe ? 0 : 2)] == 0xAB);
    const auto changed = std::inner_product(file.begin(), file.end(), edited.begin(), std::size_t{0}, std::plus<>(),
                                            [](uint8_t a, uint8_t b) { return a != b ? 1u : 0u; });
    XSFLIB_CHECK(changed <= 2 + 4 + 3 + 4 + 5);
    XSFLIB_CHECK(NoteWaves(read) == NoteWaves(Read(file, flavor)));

    // A sound without places takes the values that the reader gives without them, and nothing else.
    const WaveSoundFile::WaveSound bare = Read(file, flavor).sounds[1];
    XSFLIB_CHECK(WithWaveSoundValues(file, flavor, 1, bare) == file);
    for (const auto change :
         {+[](WaveSoundFile::WaveSound& v) { v.pan = 10; }, +[](WaveSoundFile::WaveSound& v) { v.pitch = 2.0f; },
          +[](WaveSoundFile::WaveSound& v) { v.low_pass = 0; },
          +[](WaveSoundFile::WaveSound& v) { v.aux_sends[2] = 1; },
          +[](WaveSoundFile::WaveSound& v) { v.adshr.release = 1; }})
    {
        WaveSoundFile::WaveSound other = bare;
        change(other);
        XSFLIB_CHECK(Throws<std::invalid_argument>([&] { WithWaveSoundValues(file, flavor, 1, other); }));
    }
}

// The bytes after the table of waves in `edited` are those of `file`, moved by `delta`, but for the notes' indices.
bool MovedTail(std::span<const uint8_t> file, std::span<const uint8_t> edited, std::size_t table_end,
               std::ptrdiff_t delta, Flavor flavor)
{
    const WaveSoundFile before = Read(file, flavor);
    const WaveSoundFile after = Read(edited, flavor);
    bool same = edited.size() == static_cast<std::size_t>(static_cast<std::ptrdiff_t>(file.size()) + delta);
    std::size_t differ = 0;
    for (std::size_t i = table_end; same && i < file.size(); ++i)
    {
        differ += file[i] != edited[static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i) + delta)];
    }

    // Each note's index takes 4 bytes. The sounds' values and events stay.
    same = same && differ <= 4 * after.sounds.size();
    for (std::size_t i = 0; same && i < after.sounds.size(); ++i)
    {
        same = before.sounds[i].pan == after.sounds[i].pan && before.sounds[i].pitch == after.sounds[i].pitch &&
               before.sounds[i].adshr == after.sounds[i].adshr && before.sounds[i].tracks.size() == 1 &&
               after.sounds[i].tracks[0][0].length == before.sounds[i].tracks[0][0].length;
    }

    return same;
}

void TestWave(Flavor flavor)
{
    const std::vector<uint8_t> file = sar_files::WaveSoundFile(flavor, Spec());
    const xsflib::Reader r(file, flavor == Flavor::kCafe);
    const std::size_t table = 0x28 + static_cast<std::size_t>(r.S32(0x2C)); // after INFO's header
    const std::size_t table_end = table + 4 + 2 * 8;
    const WaveId first{0x05000001, 3};
    const WaveId second{0x05000002, 0};
    const WaveId fresh{0x05000003, 5};

    // A wave of the table, a new wave, and the same wave again.
    const std::vector<uint8_t> shrunk = WithWaveSoundWave(file, flavor, 1, 0, first);
    const std::vector<uint8_t> grown = WithWaveSoundWave(file, flavor, 0, 0, fresh);
    const std::vector<uint8_t> same = WithWaveSoundWave(file, flavor, 2, 0, first);

    // The table lists the notes' waves in the order of their first use.
    XSFLIB_CHECK(r.U16(0x28) == 0x0100 && r.U32(table) == 2);
    XSFLIB_CHECK(Read(shrunk, flavor).waves == std::vector<WaveId>({first}));
    XSFLIB_CHECK(NoteWaves(Read(shrunk, flavor)) == std::vector<WaveId>({first, first, first}));
    XSFLIB_CHECK(MovedTail(file, shrunk, table_end, -8, flavor));
    XSFLIB_CHECK(Read(grown, flavor).waves == std::vector<WaveId>({fresh, second, first}));
    XSFLIB_CHECK(NoteWaves(Read(grown, flavor)) == std::vector<WaveId>({fresh, second, first}));
    XSFLIB_CHECK(MovedTail(file, grown, table_end, 8, flavor));
    XSFLIB_CHECK(same == file);

    // The headers' sizes follow.
    const xsflib::Reader g(grown, flavor == Flavor::kCafe);
    XSFLIB_CHECK(g.U32(0x0C) == grown.size() && g.U32(0x1C) == grown.size() - 0x20 && g.U32(0x24) == g.U32(0x1C));

    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { WithWaveSoundWave(file, flavor, 0, 1, fresh); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { WithWaveSoundWave(file, flavor, 3, 0, fresh); }));
}

} // namespace

int main()
{
    for (const Flavor flavor : {Flavor::kCafe, Flavor::kNx})
    {
        TestPlaces(flavor);
        TestValues(flavor);
        TestWave(flavor);
    }

    return xsflib::testing::Summary("wave sound edit");
}
