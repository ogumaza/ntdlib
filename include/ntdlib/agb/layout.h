// SPDX-License-Identifier: MIT

// What the cuts and the joins of the GBA drivers share (each driver's layout.h): the spans of a ROM, the bytes that a
// driver reads, the free space at the ROM's end where the sequences that move go, the runs of filler bytes inside the
// ROM, and the steps that lay a ROM's pieces out again.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ntdlib/agb/encoded.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::agb
{

// The largest ROM that the GBA maps: 32 MiB.
inline constexpr uint32_t kLargestRom = 0x02000000;

// The bytes that the free space and a filler run leave clear of the data before them: data that happens to end with the
// run's byte. A filler run also ends this many bytes before its run's end.
inline constexpr uint32_t kFreeSpaceMargin = 256;

// `size` bytes of a ROM from `start`, counted from the ROM's start.
struct Span
{
    friend bool operator==(const Span&, const Span&) = default;

    uint32_t start = 0;
    uint32_t size = 0;
};

// Where the sequences that move can go, besides the places of the sequences that move.
struct FreeSpace
{
    friend bool operator==(const FreeSpace&, const FreeSpace&) = default;

    // Where the free space at the ROM's end starts, from the ROM's start. It is 256 bytes or more into the run of 0xFF
    // or of zeros at the ROM's end, on a multiple of 256 bytes, and past every byte of that run that the driver reads.
    // Without such a run, it is at the ROM's end, on a multiple of 4 bytes. It can be past the ROM's end.
    uint32_t start = 0;
    uint8_t padding = 0xFF;   // what fills the bytes that the ROM grows by, up to a sequence
    std::vector<Span> filler; // the driver's FillerRuns
};

// The bytes of a ROM that a driver reads.
class Reached
{
public:
    explicit Reached(const Rom& rom);

    // Whether the driver reads the byte at `offset` from the ROM's start.
    bool Holds(uint32_t offset) const;

    // Whether the driver reads any of the `size` bytes from `address` that are in the ROM.
    bool Touches(uint32_t address, uint32_t size) const;

    // The offset just past the last byte that the driver reads past the ROM's end and before kLargestRom, or 0.
    uint64_t Beyond() const;

    // Marks `size` bytes from `address` where they are in the ROM, and notes those past its end.
    void Mark(uint32_t address, uint64_t size);

private:
    std::vector<bool> bytes_;
    uint64_t beyond_ = 0;
};

// `value` rounded up to a multiple of `alignment`.
uint64_t AlignUp(uint64_t value, uint64_t alignment);

// Where the run of `byte` at the end of `rom` starts, from the ROM's start. Returns the ROM's size when its last byte
// isn't `byte`.
uint32_t EndRunOf(const Rom& rom, uint8_t byte);

// The free space at the end of `rom`, without filler runs. Where the ROM ends with a run of 0xFF or of zeros, the free
// space starts kFreeSpaceMargin bytes or more past the last byte of the run that `reached` holds, and past the offset
// `floor`, on a multiple of kFreeSpaceMargin. The padding is the run's byte. Without such a run, the free space starts
// at the ROM's end or at `floor`, whichever is later, on a multiple of 4 bytes. `reached` can be null: the driver then
// reads none of the run.
FreeSpace FreeSpaceOf(const Rom& rom, const Reached* reached, uint64_t floor = 0);

// The runs of filler bytes in `rom` that each driver's FillerRuns returns: runs of 0xFF in the order of their places,
// then, with `zeros`, runs of zeros in the order of their places. A filler run is in a run of `least_run` or more bytes
// of 0xFF or of zeros that contains no byte that `reached` holds. A run at the ROM's end doesn't count. The filler run
// starts kFreeSpaceMargin bytes or more into the run, on a multiple of kFreeSpaceMargin, and ends kFreeSpaceMargin
// bytes before the run's end.
std::vector<Span> FillerRunsOf(const Rom& rom, const Reached& reached, uint32_t least_run, bool zeros = true);

// Adds the ROM's bytes from `at` to `end` to `pieces` as a run, and moves `at` to `end`. Adds nothing unless `end` is
// past `at`.
void AddRomRun(const Rom& rom, uint32_t& at, uint32_t end, std::vector<sar::Piece>& pieces);

// The place in `pieces` of `address`, in one of their runs. The pieces lay the ROM out from its base. Returns
// std::nullopt when no run contains the address.
std::optional<sar::Place> RunPlaceOf(std::span<const sar::Piece> pieces, uint32_t address);

// The rules of a driver's join (JoinedRom).
struct JoinRules
{
    // A sequence that moves starts on a multiple of this.
    uint32_t alignment = 1;

    // Whether a sequence that moves stays after each place in a run that a field counts from: the offsets of the
    // driver's tables are positive.
    bool after_tables = false;

    // Whether the layout has to start with a run.
    bool run_first = false;

    // Whether the sequences that move can go in the filler runs where they don't fit in the largest ROM.
    bool filler = false;

    // What the messages call the sequences, such as "songs".
    std::string_view sequences = "sequences";
};

// A ROM laid out again from a driver's pieces, before the driver writes its fields. Each run goes to its place, and
// each sequence piece is encoded. A sequence that fits in its piece's room stays there. The others move, the largest
// first and the earlier of two of a size: to the rooms of the sequences that move, where neighbours count as one, then
// to the free space, then, with JoinRules::filler, to the filler runs. The constructor throws std::invalid_argument for
// pieces that a GBA ROM can't hold and for sequences that don't fit.
class JoinedRom
{
public:
    // Encodes the sequence piece at index `piece`, the piece of the sequence at index `sequence`.
    using Encode = std::function<Encoded(std::size_t piece, uint32_t sequence)>;

    // `sequences` is the number of the layout's sequences.
    JoinedRom(std::span<const sar::Piece> pieces, std::size_t sequences, std::span<const sar::Field> fields,
              const FreeSpace& free, const JoinRules& rules, const Encode& encode);

    // The place of `place`, from the ROM's start. Throws std::invalid_argument for a place that the pieces don't have.
    uint64_t Position(const sar::Place& place) const;

    // The `width` bytes of the ROM where `field` goes. Throws std::invalid_argument when they aren't in a run.
    uint8_t* FieldBytes(const sar::Field& field, uint32_t width);

    // The ROM's bytes, moved out.
    std::vector<uint8_t> TakeImage();

private:
    std::span<const sar::Piece> pieces_;
    std::vector<uint64_t> starts_;             // each piece's place, from the ROM's start
    std::vector<std::vector<uint64_t>> items_; // the places of the items of each sequence piece
    std::vector<uint8_t> image_;
};

// The items of each piece that `fields` point to, in order and once each.
std::vector<std::vector<uint32_t>> NamedItems(std::span<const sar::Piece> pieces, std::span<const sar::Field> fields);

} // namespace ntdlib::agb
