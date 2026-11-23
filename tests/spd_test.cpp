// SPDX-License-Identifier: MIT

// Tests of the reader of Nintendo SPD's sound data (ntdlib/spd/song.h) and of the search of a cartridge image for it
// (ntdlib/spd/driver.h). The blocks are made up with ntdlib/testing/spd_files.h.

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "seqlib/testing/check.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/rom.h"
#include "ntdlib/spd/driver.h"
#include "ntdlib/spd/loops.h"
#include "ntdlib/spd/player.h"
#include "ntdlib/spd/profile.h"
#include "ntdlib/spd/song.h"
#include "ntdlib/testing/nds_files.h"
#include "ntdlib/testing/spd_files.h"

using xsflib::testing::Check;

namespace
{

namespace spd = ntdlib::spd;
using ntdlib::testing::SpdBlock;
using ntdlib::testing::SpdBlockBytes;
using ntdlib::testing::SpdEvent;
using ntdlib::testing::SpdInstrument;
using ntdlib::testing::SpdSound;
using seqlib::testing::Throws;

constexpr uint32_t kEnvelope = 0x113CA062; // bend range 2, attack 3, decay 74, sustain 40/64, release 8
constexpr uint32_t kSampleKind = 3;
constexpr uint32_t kKitKind = 7;

// A block with one program: instrument 0 plays sample 0, and instrument 2 is kit 0. Kit 0 plays key 36 at a fixed key
// of 50 and a pan of 100, and key 38 at key 38.
SpdBlock MakeBlock(std::vector<SpdSound> sounds)
{
    SpdBlock block;
    block.samples.push_back({0x48, 66, 761, 3, 2});
    block.programs.push_back({{0, kEnvelope, kSampleKind}, {0, 0, kKitKind}});
    block.program_maps.push_back({0, 0xFF, 1});
    block.kits.push_back(
        {{0, kEnvelope, kSampleKind | (50U << 17) | (100U << 25)}, {0, kEnvelope, kSampleKind | (0xFFU << 17)}});
    block.kit_maps.push_back({0, 0xFF, 1});
    block.sounds = std::move(sounds);
    return block;
}

std::vector<uint16_t> Note(uint8_t key, uint8_t velocity, uint16_t length)
{
    return {SpdEvent(key, velocity), length};
}

std::vector<uint16_t> Join(std::initializer_list<std::vector<uint16_t>> parts)
{
    std::vector<uint16_t> out;
    for (const std::vector<uint16_t>& part : parts)
    {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// The opening of a track: a tempo, program 0 and its instrument `index`.
std::vector<uint16_t> Opening(uint8_t tempo, uint8_t index = 0)
{
    return {SpdEvent(0xF0, tempo), SpdEvent(0xF2, 0), SpdEvent(0xF1, index)};
}

std::vector<uint16_t> Wait(uint8_t ticks)
{
    return {SpdEvent(0x80, ticks)};
}

const std::vector<uint16_t> kEnd = {SpdEvent(0xFF, 0)};

void TestReading()
{
    const std::vector<uint8_t> bytes = SpdBlockBytes(MakeBlock({{0x7F0, {kEnd}}}));
    const spd::Block block(bytes);
    Check(block.ProgramCount() == 1 && block.KitCount() == 1 && block.SoundCount() == 1, "a block's counts");
    const std::optional<spd::Instrument> first = block.ProgramInstrument(0, 0);
    Check(first && first->Kind() == spd::InstrumentKind::kSample, "a program's first instrument plays a sample");
    Check(!block.ProgramInstrument(0, 1) && !block.ProgramInstrument(1, 0) && !block.ProgramInstrument(0, 200),
          "a map's empty entry, a missing program and an index past the map name no instrument");
    const std::optional<spd::Instrument> kit = block.ProgramInstrument(0, 2);
    Check(kit && kit->Kind() == spd::InstrumentKind::kKit && kit->Kit() == 0, "instrument 2 is kit 0");
    const spd::Envelope envelope = first->EnvelopeOf();
    Check(envelope.bend_range == 2 && envelope.attack == 3 && envelope.decay == 74 && envelope.sustain == 40 &&
              envelope.release == 8,
          "an envelope's fields");
    const spd::Sample sample = block.SampleAt(first->sample);
    Check(sample.Format() == spd::SampleFormat::kImaAdpcm && sample.Loops() && sample.root_key == 66 &&
              sample.loop_start == 3 && sample.loop_length == 2 && sample.data.size() == 20 &&
              sample.data.data() == bytes.data() + first->sample + 8,
          "a sample's header, and its data from the header's byte 8");
    // Timer 761 plays a period of 760: 761 * 65535 / 65536, rounded down, and 0x10000 - 760 is a multiple of 4.
    Check(std::abs(sample.Rate() - 33513982.0 / 2.0 / 760.0) < 1e-6, "a sample's rate comes from its timer");
    const std::optional<spd::Instrument> fixed = block.KitInstrument(0, 36);
    Check(fixed && fixed->FixedKey() == 50 && fixed->KitPan() == 100, "a kit's key with a fixed key and pan");
    const std::optional<spd::Instrument> plain = block.KitInstrument(0, 38);
    Check(plain && !plain->FixedKey(), "a kit's key without a fixed key");
    Check(!block.KitInstrument(0, 37) && !block.KitInstrument(0, 35) && !block.KitInstrument(0, 108),
          "a kit's empty entry and keys outside 36 to 107 name no instrument");
    const std::optional<spd::Sound> sound = block.SoundAt(0);
    Check(sound && sound->flags == 0x7F0 && sound->tracks.size() == 1 && !block.SoundAt(1), "the sound table");
    Check(spd::PlayerOf(0x37F4) == 4 && spd::VolumeOf(0x37F4) == 127, "a sound's player and volume");

    std::vector<uint8_t> file(0x400, 0);
    const char magic[] = "SDFDAT";
    std::copy(magic, magic + 6, file.begin());
    ntdlib::testing::PutLe16(file, 8, 2);
    ntdlib::testing::PutLe32(file, 0x14, 0x400);
    ntdlib::testing::PutLe32(file, 0x18, static_cast<uint32_t>(bytes.size()));
    ntdlib::testing::PutLe32(file, 0x24, 0x400);
    ntdlib::testing::PutLe32(file, 0x28, static_cast<uint32_t>(bytes.size()));
    file.insert(file.end(), bytes.begin(), bytes.end());
    const std::vector<spd::BlockPlace> places = spd::ReadBlocks(file);
    Check(places.size() == 2 && places[0].slot == 0 && places[1].slot == 2 && places[1].offset == 0x400,
          "the blocks of a sound data file, empty slots left out");
    file[0] = 'X';
    const bool threw = Throws<xsflib::FormatError>([&] { spd::ReadBlocks(file); });
    Check(threw, "a file without the magic word is refused");
}

void TestDriver()
{
    const std::vector<uint16_t> track = Join({Opening(160), Note(60, 100, 2), Wait(2), kEnd});
    const std::vector<uint8_t> set_a = SpdBlockBytes(MakeBlock({{0x7F0, {track}}, {0x7F0, {track}}}));
    const std::vector<uint8_t> set_b = SpdBlockBytes(MakeBlock({{0x7F0, {track}}}));
    namespace nds = ntdlib::testing::nds_files;
    nds::Image spec;
    spec.files = {{"data/other.bin", std::vector<uint8_t>(64, 0x55)},
                  {"sounddata/sounddata.bin", ntdlib::testing::SpdSoundData({set_a, {}, set_b})}};
    const std::vector<uint8_t> image = nds::BuildImage(spec).first;
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const std::optional<spd::DriverInfo> info = spd::FindDriver(rom);
    Check(info && info->path == "sounddata/sounddata.bin", "the sound data file");
    Check(info && info->blocks.size() == 2 && info->blocks[1].slot == 2, "an empty slot gives no block");
    Check(info && info->songs.size() == 3 && info->songs[2].block == 1 && info->songs[2].sound == 0,
          "the songs, block by block");
    if (info)
    {
        Check(spd::SongName(*info, 2) == "02_000", "a song's name gives the slot and the sound");
        const std::vector<spd::Block> blocks = spd::ReadBlockData(spd::SoundData(rom, *info), *info);
        const spd::Sets first = spd::SetsOf(blocks, *info, 0);
        const spd::Sets last = spd::SetsOf(blocks, *info, 2);
        Check(first.a == &blocks[0] && first.b == nullptr && last.b == &blocks[1],
              "block 0's songs play set A alone, and the others with their block as set B");
    }

    nds::Image without;
    without.files = {{"data/other.bin", std::vector<uint8_t>(64, 0x55)}};
    const std::vector<uint8_t> other = nds::BuildImage(without).first;
    Check(!spd::FindDriver(ntdlib::ntr::Rom::Parse(other)), "an image without sound data has no driver");
}

// The length of a sound on the model (loops.h). At tempo 160, a tick lasts a frame.
void TestMeasure()
{
    const std::vector<uint16_t> looping =
        Join({Opening(160), {SpdEvent(0xF8, 0)}, Note(60, 127, 4), Wait(8), {SpdEvent(0xF9, 0)}});
    const std::vector<uint16_t> ending = Join({Opening(160), Note(60, 127, 2), Wait(2), kEnd});
    const std::vector<uint16_t> holding = Join({Opening(160), Note(60, 127, 0), Wait(2), kEnd});
    const std::vector<uint8_t> bytes =
        SpdBlockBytes(MakeBlock({{0x7F0, {looping}}, {0x7F0, {ending}}, {0x000, {ending}}, {0x7F0, {holding}}}));
    const spd::Block block(bytes);
    const std::vector<uint8_t> file = ntdlib::testing::SpdSoundData({bytes});
    const spd::Layout layout = spd::GameLayout(file);
    const auto measure = [&](int sound)
    {
        spd::Player player(file, block, nullptr, 0, layout);
        return spd::MeasureSound(player, 0, sound, 1000);
    };

    const spd::SoundLength loop = measure(1);
    Check(loop.loop_start == 0u && loop.loop_frames == 8 && !loop.end && loop.audible,
          "a loop from the frame of 0xF8 to the frame of 0xF9");
    const spd::SoundLength end = measure(2);
    Check(!end.loop_start && end.end && *end.end > 2 && end.audible, "a sound that ends after its note's release");
    Check(!measure(3).audible, "a sound at volume 0 plays nothing");
    const spd::SoundLength hold = measure(4);
    Check(
        hold.held && !hold.end && !hold.loop_start && hold.audible,
        "a note with no length on a looping wave holds once its track has ended and its envelope reached the sustain");
}

// A sound whose start action starts another sound on its player plays that sound: RedirectOf() names it.
void TestRedirect()
{
    const std::vector<uint16_t> track = Join({Opening(160), Note(60, 100, 2), Wait(2), kEnd});
    const std::vector<uint16_t> other = Join({Opening(160), Note(64, 100, 2), Wait(2), kEnd});
    std::vector<uint8_t> bytes = SpdBlockBytes(MakeBlock({{0x7F0 | 1U << 20, {kEnd}},
                                                          {0x7F0, {track}},
                                                          {0x7F0 | 1U << 20, {other}},
                                                          {0x7F0 | 2U << 20, {kEnd}},
                                                          {0x7F1, {track}}}));
    ntdlib::testing::PutLe32(bytes, spd::kEndActionsOffset, spd::StartMessage(0, 2));
    ntdlib::testing::PutLe32(bytes, spd::kEndActionsOffset + 4, spd::StartMessage(0, 5));
    const std::vector<uint8_t> file = ntdlib::testing::SpdSoundData({bytes});
    const std::vector<spd::Block> blocks = {spd::Block(bytes)};
    spd::DriverInfo info;
    info.blocks = {{0, 0, static_cast<uint32_t>(bytes.size())}};
    info.songs = {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}};

    Check(spd::RedirectOf(file, blocks, info, 0) == 1, "a sound that only starts another names it");
    Check(spd::RedirectOf(file, blocks, info, 2) == 1, "the start action replaces the sound's tracks at once");
    Check(!spd::RedirectOf(file, blocks, info, 1), "a sound without a start action");
    Check(!spd::RedirectOf(file, blocks, info, 3), "a start on another player layers the two sounds");
}

} // namespace

int main()
{
    TestReading();
    TestDriver();
    TestMeasure();
    TestRedirect();

    return xsflib::testing::Summary("spd");
}
