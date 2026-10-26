// SPDX-License-Identifier: MIT

// Tests of the model of the Wii's wave sound files (ntdlib/rsar/wsd_file.h). The offsets that the tests expect follow
// from the layout that the header describes. The reader of formats.h reads the written files back.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace rsar = ntdlib::rsar;
using rsar::WaveEncoding;
using rsar::WaveFile;
using rsar::WsdFile;

rsar::ReadOptions PlaybackOptions()
{
    rsar::ReadOptions options;
    options.playback_fields = true;

    return options;
}

// A note whose values follow from `n`. It plays wave n % 2.
WsdFile::Note NoteOf(int n)
{
    WsdFile::Note note;
    note.wave_index = n % 2;
    note.attack = static_cast<uint8_t>(120 - n);
    note.decay = 110;
    note.sustain = 100;
    note.release = 90;
    note.hold = static_cast<uint8_t>(n);
    note.original_key = static_cast<uint8_t>(60 + n);
    note.volume = static_cast<uint8_t>(127 - n);
    note.pan = static_cast<uint8_t>(64 + n);
    note.surround_pan = static_cast<uint8_t>(n);
    note.pitch = 1.0f + static_cast<float>(n) / 4.0f;

    return note;
}

// Two wave sounds: one of a track with one event and one note, and one of two tracks and two notes.
std::vector<WsdFile::WaveSound> TwoSounds()
{
    std::vector<WsdFile::WaveSound> sounds(2);
    sounds[0].pitch = 1.5f;
    sounds[0].pan = 30;
    sounds[0].surround_pan = 5;
    sounds[0].fx_sends = {1, 2, 3};
    sounds[0].main_send = 100;
    sounds[0].tracks = {{{{0.0f, 0.0f, 0}}}};
    sounds[0].notes = {NoteOf(0)};
    sounds[1].tracks = {{{{0.0f, 0.0f, 0}, {0.5f, 2.0f, 1}}}, {{{0.25f, 0.0f, 0}}}};
    sounds[1].notes = {NoteOf(1), NoteOf(2)};

    return sounds;
}

// A mono DSP-ADPCM wave of 30 samples and a stereo PCM16 wave of 10.
std::vector<WaveFile> TwoWaves()
{
    std::vector<WaveFile> waves(2);
    waves[0].loop_end = 30;
    waves[0].channels.push_back({std::vector<uint8_t>(24, 0x11), rsar::DspAdpcmInfo{}});
    waves[1].encoding = WaveEncoding::kPcm16;
    waves[1].loop_end = 10;
    waves[1].channels.push_back({std::vector<uint8_t>(20, 0x22), std::nullopt});
    waves[1].channels.push_back({std::vector<uint8_t>(20, 0x33), std::nullopt});

    return waves;
}

WsdFile TwoSoundsFile(uint16_t version)
{
    WsdFile wsd;
    wsd.version = version;
    wsd.sounds = TwoSounds();
    wsd.waves = TwoWaves();

    return wsd;
}

// Whether the reference at `at` of `r` names `offset`.
bool Names(const xsflib::Reader& r, std::size_t at, uint32_t offset)
{
    return r.U32(at) == 0x01000000 && r.U32(at + 4) == offset;
}

// Each wave sound's references, settings, table of tracks with each track's table of events and the events, then its
// table of notes and the notes, in pre-order after the references to the wave sounds.
void TestLayout()
{
    const WsdFile wsd = TwoSoundsFile(0x0103);
    const WsdFile::Files files = wsd.Write();
    const xsflib::Reader r(files.file, true);
    const std::size_t base = 0x28;

    XSFLIB_CHECK(files.file.size() == 0x200 && r.Magic(0, "RWSD") && r.U16(4) == 0xFEFF && r.U16(6) == 0x0103);
    XSFLIB_CHECK(r.U32(8) == 0x200 && r.U16(0xC) == 0x20 && r.U16(0xE) == 1 && r.U32(0x10) == 0x20);
    XSFLIB_CHECK(r.U32(0x14) == 0x1E0 && r.U32(0x18) == 0 && r.Magic(0x20, "DATA") && r.U32(0x24) == 0x1E0);
    XSFLIB_CHECK(r.U32(base) == 2 && Names(r, base + 4, 0x14) && Names(r, base + 0xC, 0xB8));

    // The first wave sound.
    XSFLIB_CHECK(Names(r, base + 0x14, 0x2C) && Names(r, base + 0x1C, 0x4C) && Names(r, base + 0x24, 0x7C));
    XSFLIB_CHECK(r.U32(base + 0x2C) == 0x3FC00000 && r.U8(base + 0x30) == 30 && r.U8(base + 0x31) == 5);
    XSFLIB_CHECK(r.U8(base + 0x32) == 1 && r.U8(base + 0x34) == 3 && r.U8(base + 0x35) == 100);
    XSFLIB_CHECK(r.U16(base + 0x36) == 0 && r.U32(base + 0x38) == 0 && r.U32(base + 0x48) == 0);
    XSFLIB_CHECK(r.U32(base + 0x4C) == 1 && Names(r, base + 0x50, 0x58) && Names(r, base + 0x58, 0x60));
    XSFLIB_CHECK(r.U32(base + 0x60) == 1 && Names(r, base + 0x64, 0x6C) && r.U32(base + 0x6C) == 0);
    XSFLIB_CHECK(r.U32(base + 0x78) == 0 && r.U32(base + 0x7C) == 1 && Names(r, base + 0x80, 0x88));
    XSFLIB_CHECK(r.S32(base + 0x88) == 0 && r.U8(base + 0x8C) == 120 && r.U8(base + 0x94) == 60);
    XSFLIB_CHECK(r.U8(base + 0x95) == 127 && r.U32(base + 0x98) == 0x3F800000 && r.U32(base + 0x9C) == 0);

    // The second: each track's table of events follows the track.
    XSFLIB_CHECK(Names(r, base + 0xB8, 0xD0) && Names(r, base + 0xC0, 0xF0) && Names(r, base + 0xC8, 0x164));
    XSFLIB_CHECK(r.U32(base + 0xD0) == 0x3F800000 && r.U8(base + 0xD4) == 64 && r.U8(base + 0xD9) == 127);
    XSFLIB_CHECK(r.U32(base + 0xF0) == 2 && Names(r, base + 0xF4, 0x104) && Names(r, base + 0xFC, 0x140));
    XSFLIB_CHECK(Names(r, base + 0x104, 0x10C) && r.U32(base + 0x10C) == 2 && Names(r, base + 0x110, 0x120));
    XSFLIB_CHECK(Names(r, base + 0x118, 0x130) && r.U32(base + 0x130) == 0x3F000000);
    XSFLIB_CHECK(r.U32(base + 0x134) == 0x40000000 && r.U32(base + 0x138) == 1 && r.U32(base + 0x13C) == 0);
    XSFLIB_CHECK(Names(r, base + 0x140, 0x148) && r.U32(base + 0x148) == 1 && Names(r, base + 0x14C, 0x154));
    XSFLIB_CHECK(r.U32(base + 0x154) == 0x3E800000 && r.U32(base + 0x164) == 2);
    XSFLIB_CHECK(Names(r, base + 0x168, 0x178) && Names(r, base + 0x170, 0x1A8));
    XSFLIB_CHECK(r.S32(base + 0x1A8) == 0 && r.U8(base + 0x1A8 + 0xC) == 62 &&
                 r.U32(base + 0x1A8 + 0x10) == 0x3FC00000);

    // The waves go into an RWAR.
    const rsar::WaveArchiveFile archive = rsar::WaveArchiveFile::Read(files.wave_data);
    XSFLIB_CHECK(archive.waves.size() == 2 && WaveFile::Read(archive.waves[0]) == TwoWaves()[0]);

    // NW4R SND's values, and the model read back.
    const rsar::WaveSoundFile parsed = rsar::WaveSoundFile::Parse(files.file, files.wave_data, PlaybackOptions());
    XSFLIB_CHECK(parsed.Version() == 0x0103 && !parsed.HasWaveBlock() && parsed.Sounds().size() == 2);
    XSFLIB_CHECK(parsed.Waves().size() == 2 && parsed.Waves()[1].channels.size() == 2);
    const rsar::WaveSoundFile::WaveSound& first = parsed.Sounds()[0];
    XSFLIB_CHECK(first.pitch == 1.5f && first.pan == 30 && first.surround_pan == 5 && first.main_send == 100);
    XSFLIB_CHECK(first.fx_sends == (std::array<uint8_t, 3>{1, 2, 3}) && first.notes.size() == 1);
    const rsar::WaveSoundFile::Note& note = parsed.Sounds()[1].notes[1];
    XSFLIB_CHECK(note.wave_index == 0 && note.attack == 118 && note.hold == 2 && note.original_key == 62);
    XSFLIB_CHECK(note.volume == 125 && note.pan == 66 && note.surround_pan == 2 && note.pitch == 1.5f);
    XSFLIB_CHECK(WsdFile::Read(files.file, files.wave_data) == wsd);
}

// A file of version 1.2 describes its waves in WAVE, by offsets from the block's start. Each wave's samples start on 32
// bytes of the wave data.
void TestWaveBlock()
{
    WsdFile wsd;
    wsd.version = 0x0102;
    wsd.sounds = {TwoSounds()[0]};
    wsd.waves = TwoWaves();
    const WsdFile::Files files = wsd.Write();
    const xsflib::Reader r(files.file, true);

    XSFLIB_CHECK(files.file.size() == 0x1C0 && r.U16(6) == 0x0102 && r.U16(0xE) == 2 && r.U32(0x14) == 0xC0);
    XSFLIB_CHECK(r.U32(0x18) == 0xE0 && r.U32(0x1C) == 0xE0 && r.Magic(0xE0, "WAVE") && r.U32(0xE4) == 0xE0);
    XSFLIB_CHECK(Names(r, 0x2C, 0xC) && Names(r, 0x34, 0x24) && Names(r, 0x3C, 0x44) && Names(r, 0x44, 0x74));
    XSFLIB_CHECK(r.U32(0xE8) == 2 && r.U32(0xEC) == 0x14 && r.U32(0xF0) == 0x80 && r.U8(0xF4) == 2);
    XSFLIB_CHECK(r.U8(0x160) == 1 && r.U32(0xF4 + 0x14) == 0 && r.U32(0x160 + 0x14) == 32 && r.U32(0x1BC) == 0);
    XSFLIB_CHECK(files.wave_data.size() == 96 && files.wave_data[32] == 0x22 && files.wave_data[52] == 0x33);

    const rsar::WaveSoundFile parsed = rsar::WaveSoundFile::Parse(files.file, files.wave_data, PlaybackOptions());
    XSFLIB_CHECK(parsed.HasWaveBlock() && parsed.WaveInfoCount() == 2 && parsed.Waves()[0].loop_end == 30);
    XSFLIB_CHECK(parsed.Waves()[1].channels[1].data.data() == files.wave_data.data() + 52);
    XSFLIB_CHECK(WsdFile::Read(files.file, files.wave_data) == wsd);
}

template <class Error, class Call>
bool Throws(Call call, const std::string& part)
{
    const std::string message = ErrorOf<Error>(call);

    return !message.empty() && message.find(part) != std::string::npos;
}

// Write refuses other versions, and waves that WaveFile refuses. Read refuses other files and versions, wave data that
// isn't the file's, and a file that the tools didn't lay out.
void TestRefusals()
{
    WsdFile older = TwoSoundsFile(0x0101);
    WsdFile bare = TwoSoundsFile(0x0102);
    bare.waves[0].channels.clear();
    const WsdFile::Files files = TwoSoundsFile(0x0103).Write();
    std::vector<uint8_t> newer = files.file;
    newer[7] = 4;
    std::vector<uint8_t> tail = files.file;
    tail[0x28 + 0x2C + 0x10] = 1;
    std::vector<uint8_t> typed = files.file;
    typed[0x28 + 0x14 + 1] = 1;
    const WsdFile::Files old = TwoSoundsFile(0x0102).Write();

    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { older.Write(); }, "a version other than 1.2 and 1.3"));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { bare.Write(); }, "holds 0 channels"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WsdFile::Read(files.wave_data, files.wave_data); }, "not an RWSD"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WsdFile::Read(newer, files.wave_data); }, "other than 1.2 and 1.3"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WsdFile::Read(tail, files.wave_data); }, "other bytes of the file"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WsdFile::Read(typed, files.wave_data); }, "of another kind"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WsdFile::Read(files.file, old.wave_data); }, "not an RWAR"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WsdFile::Read(old.file, {}); }, "start past the wave data"));
    XSFLIB_CHECK(WsdFile::Read(old.file, old.wave_data) == TwoSoundsFile(0x0102));
}

} // namespace

int main()
{
    TestLayout();
    TestWaveBlock();
    TestRefusals();

    return xsflib::testing::Summary("Wii wave sound file");
}
