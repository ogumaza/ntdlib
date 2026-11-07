// SPDX-License-Identifier: MIT

// Tests of the cut and the join of a ROM of Rare's GBA driver (ntdlib/rare/layout.h). The ROMs are built by hand: a
// tune table, the tunes' headers, track lists, program maps and instruments, a sample and the tracks.

#include <cstdint>
#include <stdexcept>
#include <variant>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rare/commands.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/rare/layout.h"
#include "ntdlib/rare/song.h"

using xsflib::testing::Check;

namespace
{

namespace rare = ntdlib::rare;
using ntdlib::agb::kRomBase;
using ntdlib::agb::Rom;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// The places in a test ROM.
constexpr uint32_t kTuneTable = 0x100;
constexpr uint32_t kHeaders = 0x200;    // 20 bytes for each tune
constexpr uint32_t kTrackLists = 0x300; // 16 bytes for each tune
constexpr uint32_t kProgramMap = 0x400;
constexpr uint32_t kInstruments = 0x480;
constexpr uint32_t kInstrument = 0x500;
constexpr uint32_t kSample = 0x600;
constexpr uint32_t kTracks = 0x800;

// A ROM with tunes whose tracks lie one after another from kTracks, and `tail` bytes of `fill` after them.
class TestRom
{
public:
    // Adds a tune whose tracks are `tracks`, in the channel-byte format. Tracks that `shared` names take the tracks of
    // that tune.
    void AddTune(const std::vector<std::vector<uint8_t>>& tracks, int shared = -1)
    {
        tunes_.push_back({tracks, shared});
    }

    std::vector<uint8_t> Build(uint32_t tail = 0x100, uint8_t fill = 0xFF) const
    {
        std::vector<uint8_t> bytes(kTracks);
        const auto put32 = [&bytes](uint32_t at, uint32_t value)
        {
            xsflib::StoreLe32(&bytes[at], value);
        };

        // An instrument of a sample of 16 bytes at 8,000 Hz, played once, for program 0.
        for (uint32_t p = 0; p < 128; p++)
        {
            bytes[kProgramMap + p] = p == 0 ? 0 : 0xFF;
        }
        put32(kInstruments, kRomBase + kInstrument);
        put32(kInstrument, 0x20);
        put32(kInstrument + 4, 1);
        put32(kInstrument + 8, 8000);
        put32(kInstrument + 12, 60);
        put32(kInstrument + 16, kRomBase + kSample);
        put32(kInstrument + 24, kRomBase + kSample + 16);
        for (uint32_t i = 0; i < 17; i++)
        {
            bytes[kSample + i] = uint8_t(i * 8);
        }

        std::vector<std::vector<uint32_t>> addresses;
        for (size_t t = 0; t < tunes_.size(); t++)
        {
            const Tune& tune = tunes_[t];
            const uint32_t header = kHeaders + 20 * uint32_t(t);
            const uint32_t list = kTrackLists + 16 * uint32_t(t);
            put32(kTuneTable + 4 * uint32_t(t), kRomBase + header);
            put32(header, uint32_t(tune.tracks.size()));
            put32(header + 4, 48);
            put32(header + 8, kRomBase + list);
            put32(header + 12, kRomBase + kProgramMap);
            put32(header + 16, kRomBase + kInstruments);

            std::vector<uint32_t> starts;
            if (tune.shared >= 0)
            {
                starts = addresses[size_t(tune.shared)];
            }
            else
            {
                for (const std::vector<uint8_t>& track : tune.tracks)
                {
                    starts.push_back(kRomBase + uint32_t(bytes.size()));
                    bytes.insert(bytes.end(), track.begin(), track.end());
                }
            }
            for (size_t k = 0; k < starts.size(); k++)
            {
                put32(list + 4 * uint32_t(k), starts[k]);
            }
            addresses.push_back(starts);
        }

        bytes.resize(bytes.size() + tail, fill);
        return bytes;
    }

private:
    struct Tune
    {
        std::vector<std::vector<uint8_t>> tracks;
        int shared = -1;
    };

    std::vector<Tune> tunes_;
};

rare::DriverInfo InfoOf(int tunes)
{
    rare::DriverInfo info;
    info.format = rare::Format::kChannelByte;
    info.tune_table = kRomBase + kTuneTable;
    info.tune_count = tunes;
    return info;
}

Rom RomOf(const std::vector<uint8_t>& bytes)
{
    Rom rom;
    rom.Assign(bytes);
    return rom;
}

// A track of a note and a wait.
std::vector<uint8_t> NoteTrack(uint8_t channel, uint8_t key)
{
    return {0x08, channel, 0, 0x05, channel, key, 100, 0x01, 48, 0x06, channel, key, 0, 0x0B};
}

void TestRoundTrip()
{
    TestRom test;
    test.AddTune({NoteTrack(0, 60), NoteTrack(1, 64)});
    test.AddTune({NoteTrack(2, 67)});
    const std::vector<uint8_t> bytes = test.Build();
    const Rom rom = RomOf(bytes);
    const rare::DriverInfo info = InfoOf(2);

    const rare::Layout layout = rare::Cut(rom, info);
    Check(layout.sequences.size() == 2, "each tune's tracks are a sequence");
    Check(layout.fields.size() == 3, "a field for each track");
    Check(rare::Join(layout) == bytes, "the pieces join to the ROM");
    Check(layout.fields[1].to.offset == 5 && layout.fields[1].to.item, "the second track starts at item 5");
    Check(layout.free.start == 0xA00, "the free space starts 256 bytes or more into the run at the end");
}

// A sequence that grows moves to the free space, and its track list names its new place. One that shrinks keeps its
// place, with zeros after it.
void TestGrowAndShrink()
{
    TestRom test;
    test.AddTune({NoteTrack(0, 60), NoteTrack(1, 64)});
    test.AddTune({NoteTrack(2, 67)});
    const std::vector<uint8_t> bytes = test.Build(0x400);
    const Rom rom = RomOf(bytes);
    const rare::DriverInfo info = InfoOf(2);
    rare::Layout layout = rare::Cut(rom, info);

    // A wait more in the first tune's first track.
    Command wait;
    wait.kind = Kind::kWait;
    wait.value.value = 10 * kOne;
    std::vector<Command>& items = layout.sequences[0].items;
    items.insert(items.begin() + 3, wait);
    for (ntdlib::sar::Field& field : layout.fields)
    {
        if (field.to.piece == layout.fields[0].to.piece && field.to.offset > 3)
        {
            field.to.offset++;
        }
    }

    const std::vector<uint8_t> grown = rare::Join(layout);
    const Rom grown_rom = RomOf(grown);
    const uint32_t first = grown_rom.U32(kRomBase + kTrackLists);
    const uint32_t second = grown_rom.U32(kRomBase + kTrackLists + 4);
    Check(first >= kRomBase + layout.free.start && first % 4 == 0, "the grown tune moves to the free space");
    Check(second == first + 16, "its second track follows the first");
    Check(grown.size() == bytes.size(), "the ROM keeps its size");
    Check(grown_rom.U32(kRomBase + kTrackLists + 16) == kRomBase + kTracks + 28, "the other tune keeps its place");

    const rare::Decoded moved = rare::DecodeRomSequence(grown_rom, info, 0);
    Check(!moved.opaque && moved.sequence.items == layout.sequences[0].items, "the moved tune reads back");

    // The ROM cuts and joins again.
    Check(rare::Join(rare::Cut(grown_rom, info)) == grown, "the grown ROM cuts and joins");

    // The first track without its note off: the tune shrinks and keeps its place.
    rare::Layout small = rare::Cut(rom, info);
    small.sequences[0].items.erase(small.sequences[0].items.begin() + 3);
    for (ntdlib::sar::Field& field : small.fields)
    {
        if (field.to.piece == small.fields[0].to.piece && field.to.offset > 3)
        {
            field.to.offset--;
        }
    }
    const Rom shrunk = RomOf(rare::Join(small));
    Check(shrunk.U32(kRomBase + kTrackLists) == kRomBase + kTracks, "the shrunk tune keeps its place");
    Check(shrunk.U32(kRomBase + kTrackLists + 4) == kRomBase + kTracks + 10, "its second track moves up");
    Check(shrunk.U8(kRomBase + kTracks + 24) == 0 && shrunk.U8(kRomBase + kTracks + 27) == 0,
          "zeros fill the rest of its place");
}

// Tunes that name the same tracks share a piece. A tune whose tracks hold the driver's other data stays in the runs.
void TestSharedAndBlocked()
{
    TestRom test;
    test.AddTune({NoteTrack(0, 60)});
    test.AddTune({NoteTrack(0, 60)}, 0);
    const std::vector<uint8_t> bytes = test.Build();
    const rare::Layout layout = rare::Cut(RomOf(bytes), InfoOf(2));
    Check(layout.sequences.size() == 1 && layout.fields.size() == 2, "two tunes share a sequence");
    Check(layout.fields[0].to == layout.fields[1].to, "both name its track");
    Check(rare::Join(layout) == bytes, "the shared tune joins");

    // A track that starts on the sample's bytes.
    std::vector<uint8_t> blocked = bytes;
    xsflib::StoreLe32(&blocked[kTrackLists], kRomBase + kSample + 15);
    blocked[kSample + 15] = 0x0B;
    const rare::Layout kept = rare::Cut(RomOf(blocked), InfoOf(1));
    Check(kept.sequences.empty() && kept.pieces.size() == 1, "a tune on a sample stays in the bytes");
    Check(rare::Join(kept) == blocked, "the ROM joins as one run");
}

// The free space at the ROM's end and the filler runs inside it.
void TestFreeSpace()
{
    TestRom test;
    test.AddTune({NoteTrack(0, 60)});
    std::vector<uint8_t> bytes = test.Build(0x3000, 0);
    const uint32_t end = uint32_t(bytes.size());
    bytes.resize(end + 0x400, 0xFF);
    bytes.back() = 0x55;
    const Rom rom = RomOf(bytes);
    const rare::Layout layout = rare::Cut(rom, InfoOf(1));
    Check(layout.free.start == ((end + 0x400 + 3) & ~3u), "a ROM without a run at its end grows from its end");
    Check(layout.free.filler.size() == 1 && layout.free.filler[0].start == 0xA00 &&
              layout.free.filler[0].start + layout.free.filler[0].size == end - 0x100,
          "the zeros after the tracks are a filler run");
    Check(rare::FillerRuns(rom, InfoOf(1)) == layout.free.filler, "FillerRuns gives the same runs");
}

// Join refuses pieces that a ROM can't hold.
void TestRefusals()
{
    TestRom test;
    test.AddTune({NoteTrack(0, 60)});
    const rare::Layout layout = rare::Cut(RomOf(test.Build()), InfoOf(1));

    rare::Layout bad = layout;
    bad.pieces.push_back(ntdlib::sar::Padding{4});
    bool threw = false;
    try
    {
        rare::Join(bad);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "padding");

    bad = layout;
    bad.fields[0].to.offset = 99;
    threw = false;
    try
    {
        rare::Join(bad);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "an entry past the items");
}

} // namespace

int main()
{
    TestRoundTrip();
    TestGrowAndShrink();
    TestSharedAndBlocked();
    TestFreeSpace();
    TestRefusals();

    return xsflib::testing::Summary("Rare's layout");
}
