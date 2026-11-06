// SPDX-License-Identifier: MIT

// Builds small JAudio2 files for tests, in the layout of Four Swords Adventures' files: wave systems (WSYS) with their
// AW file, banks (IBNK) of both versions, sound tables with their names (BST and BSTN), sequence archives (RARC) and
// start-up archives (BAA). It also builds JAudio1's start-up archive (AAF) in The Wind Waker's layout, the bank file
// of Pikmin's JAudio (.bx) and the sequence index (BARC). It writes a small JAudio2 game's files too. The GameCube's
// formats are big-endian.
// The builders with a `big_endian` parameter also build the little-endian files of the Switch's releases of GameCube
// and Wii games, such as Super Mario Galaxy's: each field in the width that the release stores it in, and each tag
// reversed.

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/files.h"

namespace ntdlib::testing::jaudio_files
{

// The builder of the files below: values appended in turn, and stores over the bytes already added. A little-endian
// file stores each value of more than a byte in reverse order, and each tag as a 32-bit word, its characters reversed.
class Bytes
{
public:
    explicit Bytes(bool big_endian = true) : big_endian_(big_endian)
    {
    }

    std::size_t Size() const
    {
        return data_.size();
    }

    Bytes& U8(uint32_t v)
    {
        data_.push_back(static_cast<uint8_t>(v));

        return *this;
    }

    Bytes& U16(uint32_t v)
    {
        return big_endian_ ? U8(v >> 8).U8(v) : U8(v).U8(v >> 8);
    }

    Bytes& U24(uint32_t v)
    {
        return big_endian_ ? U8(v >> 16).U8(v >> 8).U8(v) : U8(v).U8(v >> 8).U8(v >> 16);
    }

    Bytes& U32(uint32_t v)
    {
        return big_endian_ ? U16(v >> 16).U16(v) : U16(v).U16(v >> 16);
    }

    Bytes& F32(float v)
    {
        return U32(std::bit_cast<uint32_t>(v));
    }

    // An offset in `width` bytes: 4, or 8 in a file whose offsets are widened.
    Bytes& Offset(std::size_t v, std::size_t width)
    {
        const auto low = static_cast<uint32_t>(v);
        if (width == 8)
        {
            return big_endian_ ? U32(0).U32(low) : U32(low).U32(0);
        }

        return U32(low);
    }

    Bytes& Tag(const char* tag)
    {
        for (int i = 0; i < 4; i++)
        {
            U8(static_cast<uint8_t>(tag[big_endian_ ? i : 3 - i]));
        }

        return *this;
    }

    Bytes& Append(const std::vector<uint8_t>& more)
    {
        data_.insert(data_.end(), more.begin(), more.end());

        return *this;
    }

    Bytes& PadTo(std::size_t size)
    {
        data_.resize(std::max(data_.size(), size));

        return *this;
    }

    // Each stores `v` at `at`, over bytes already added.
    void Put8(std::size_t at, uint32_t v)
    {
        data_[at] = static_cast<uint8_t>(v);
    }

    void Put16(std::size_t at, uint32_t v)
    {
        big_endian_ ? xsflib::StoreBe16(data_.data() + at, static_cast<uint16_t>(v))
                    : xsflib::StoreLe16(data_.data() + at, static_cast<uint16_t>(v));
    }

    void Put24(std::size_t at, uint32_t v)
    {
        Put8(at + (big_endian_ ? 0 : 2), v >> 16);
        Put8(at + 1, v >> 8);
        Put8(at + (big_endian_ ? 2 : 0), v);
    }

    void Put32(std::size_t at, uint32_t v)
    {
        big_endian_ ? xsflib::StoreBe32(data_.data() + at, v) : xsflib::StoreLe32(data_.data() + at, v);
    }

    // Stores an offset over one that Offset added, `width` bytes wide.
    void PutOffset(std::size_t at, std::size_t v, std::size_t width)
    {
        Put32(width == 8 && big_endian_ ? at + 4 : at, static_cast<uint32_t>(v));
    }

    // Stores `tag` and `size` at the start, in the big-endian order whatever the file's order. A file of the Switch's
    // release of Pikmin keeps its first tag and its size that way.
    void PutBigEndianHeader(const char* tag, uint32_t size)
    {
        std::copy_n(tag, 4, data_.begin());
        xsflib::StoreBe32(data_.data() + 4, size);
    }

    const std::vector<uint8_t>& Data() const
    {
        return data_;
    }

private:
    bool big_endian_;
    std::vector<uint8_t> data_;
};

// The scene of a wave system that WaveSystemFile builds: its external mode, its dependencies, and the words of its
// lists C-EX and C-ST. Its C-DF gives wave i of the archive the word (system << 16) | i. With `null_cdf`, the offset of
// C-DF is null. So is the offset of a list without a value.
struct SceneSpec
{
    uint32_t external_mode = 0;
    std::vector<uint32_t> dependencies;
    bool null_cdf = false;
    std::optional<std::vector<uint32_t>> cex = std::vector<uint32_t>();
    std::optional<std::vector<uint32_t>> cst = std::vector<uint32_t>();
};

// A wave system with one wave archive, `archive_name`, of `waves` PCM16 waves of 64 samples each at 32000 Hz, key 60.
// The first loops over its last 32 samples. The archive's name has at most 111 characters. The archive's record holds
// the number of its waves at `wave_count_at`: 0x70, or 0x8c in the wide records of the Switch's releases of Pikmin and
// Pikmin 2. Each offset takes `offset_bytes` bytes: 4, or 8 in a little-endian system of the Switch's release of
// Pikmin. Such a system keeps its tag and its size in the big-endian order. Its size is the big-endian system's. The
// group's scene is `scene`. Its lists follow it, each with the words that its entries point to.
inline std::vector<uint8_t> WaveSystemFile(uint32_t id, int waves, const std::string& archive_name = "test.aw",
                                           bool big_endian = true, std::size_t wave_count_at = 0x70,
                                           std::size_t offset_bytes = 4, const SceneSpec& scene = {})
{
    const std::size_t width = offset_bytes;
    Bytes w(big_endian);
    w.Tag("WSYS").U32(0).U32(id).U32(0);    // size, id, table size
    w.Offset(0x20, width).Offset(0, width); // WINF, WBCT (set below)
    w.PadTo(0x20);

    const std::size_t winf = w.Size();
    w.Tag("WINF").U32(1).Offset(0, width); // one archive
    const std::size_t archive = w.Size();
    w.PutOffset(winf + 8, archive, width);

    std::vector<uint8_t> name(wave_count_at);
    std::copy_n(archive_name.begin(), std::min<std::size_t>(archive_name.size(), 0x6f), name.begin());
    w.Append(name).U32(static_cast<uint32_t>(waves));

    const std::size_t offsets = w.Size();
    for (int i = 0; i < waves; i++)
    {
        w.Offset(0, width);
    }

    for (int i = 0; i < waves; i++)
    {
        w.PutOffset(offsets + width * static_cast<std::size_t>(i), w.Size(), width);
        const bool loop = i == 0;
        w.U8(0).U8(3).U8(60).U8(0).F32(32000.0f);
        w.U32(static_cast<uint32_t>(128 * i)).U32(128); // offset and size in the AW file
        w.U32(loop ? 0xffffffff : 0).U32(loop ? 32 : 0).U32(loop ? 64 : 0).U32(64);
        w.U16(0).U16(0);
    }

    // WBCT: one scene, with the offsets of its lists, then the numbers of its dependencies.
    const std::size_t wbct = w.Size();
    w.PutOffset(0x10 + width, wbct, width);
    w.Tag("WBCT").U32(0).U32(1).Offset(wbct + 12 + width, width);
    w.Tag("SCNE").U32(scene.external_mode).U32(static_cast<uint32_t>(scene.dependencies.size()));
    const std::size_t lists = w.Size();
    w.Offset(0, width).Offset(0, width).Offset(0, width); // C-DF, C-EX and C-ST (set below)
    for (const uint32_t dependency : scene.dependencies)
    {
        w.U32(dependency);
    }

    // Each list: its tag, the number of its words, an offset of each word, then the words.
    const auto list = [&w, width](std::size_t offset_at, const char* tag, const std::vector<uint32_t>& words)
    {
        w.PutOffset(offset_at, w.Size(), width);
        w.Tag(tag).U32(static_cast<uint32_t>(words.size()));
        const std::size_t entries = w.Size();
        for (std::size_t i = 0; i < words.size(); i++)
        {
            w.Offset(0, width);
        }

        for (std::size_t i = 0; i < words.size(); i++)
        {
            w.PutOffset(entries + width * i, w.Size(), width);
            w.U32(words[i]);
        }
    };

    std::vector<uint32_t> cdf;
    for (int i = 0; i < waves; i++)
    {
        cdf.push_back(id << 16 | static_cast<uint32_t>(i));
    }

    if (!scene.null_cdf)
    {
        list(lists, "C-DF", cdf);
    }
    if (scene.cex)
    {
        list(lists + width, "C-EX", *scene.cex);
    }
    if (scene.cst)
    {
        list(lists + 2 * width, "C-ST", *scene.cst);
    }

    w.Put32(4, static_cast<uint32_t>(w.Size()));
    if (width == 8)
    {
        const std::size_t size = WaveSystemFile(id, waves, archive_name, true, 0x70, 4, scene).size();
        w.PutBigEndianHeader("WSYS", static_cast<uint32_t>(size));
    }

    return w.Data();
}

// A version 0 bank whose instrument 0 plays wave 0 on every key, with an instant attack, a hold and a release of
// `release` units. Drum set 0 (program 0xe4) plays wave 1 on key 36, panned right. A little-endian bank has the layout
// of the Switch's release of Pikmin 2: its drum table at 0x544, not 0x3b4. Its size leaves out the 0x190 bytes that
// the table moves by. With `offset_bytes` 8, a little-endian bank has the layout of the Switch's release of Pikmin:
// each offset in 8 bytes, and the drum table at 0x744. Its tag and its size keep the big-endian order, and its size is
// the big-endian bank's. Each wave id holds `wave_system` in its high half. Pikmin's bank file names a wave's system
// there. The instrument's record holds `flags` in its word at +4.
inline std::vector<uint8_t> BankV0File(uint32_t number, int16_t release, bool big_endian = true,
                                       std::size_t offset_bytes = 4, uint16_t wave_system = 0, uint32_t flags = 0)
{
    const uint32_t system = uint32_t{wave_system} << 16;
    const std::size_t width = offset_bytes;
    const std::size_t drum_table = width == 8 ? 0x744 : big_endian ? 0x3b4 : 0x544;
    Bytes b(big_endian);
    b.Tag("IBNK").U32(0).U32(number).U32(0).PadTo(0x20);
    b.Tag("BANK").PadTo(drum_table + 12 * width);

    // Oscillator tables.
    const std::size_t attack = b.Size();
    b.U16(0).U16(0).U16(0x7fff).U16(14).U16(0).U16(0);
    const std::size_t release_table = b.Size();
    b.U16(0).U16(static_cast<uint16_t>(release)).U16(0).U16(15).U16(0).U16(0);
    const std::size_t osc = b.Size();
    b.U8(0).U8(0).U16(0).F32(1.0f).Offset(attack, width).Offset(release_table, width);
    b.F32(1.0f).F32(0.0f);

    // A velocity region, a key region and the instrument.
    const std::size_t vmap = b.Size();
    b.U8(127).U8(0).U16(0).U32(system | 0).F32(1.0f).F32(1.0f);
    const std::size_t keymap = b.Size();
    b.U8(127).U8(0).U16(0).U32(1).Offset(vmap, width);
    const std::size_t inst = b.Size();
    b.Tag("INST").U32(flags).F32(1.0f).F32(1.0f).Offset(osc, width).Offset(0, width);
    b.Offset(0, width).Offset(0, width).Offset(0, width).Offset(0, width).U32(1).Offset(keymap, width);
    b.PutOffset(0x24, inst, width);

    // The drum set.
    const std::size_t drum_vmap = b.Size();
    b.U8(127).U8(0).U16(0).U32(system | 1).F32(0.5f).F32(1.0f);
    const std::size_t pmap = b.Size();
    b.F32(1.0f).F32(2.0f).Offset(0, width).Offset(0, width).U32(1).Offset(drum_vmap, width);
    const std::size_t perc = b.Size();
    const std::size_t pans = perc + 0x88 + width * 128;
    b.Tag("PER2").PadTo(pans + 128 + 256);
    b.PutOffset(perc + 0x88 + width * 36, pmap, width);
    b.Put8(pans + 36, 127);            // pan
    b.Put16(pans + 128 + 2 * 36, 200); // release: 200 units

    b.PutOffset(drum_table, perc, width);
    b.Put32(4, static_cast<uint32_t>(b.Size() - (drum_table - 0x3b4)));
    if (width == 8)
    {
        b.PutBigEndianHeader("IBNK", static_cast<uint32_t>(BankV0File(number, release).size()));
    }

    return b.Data();
}

// A version 1 bank whose program 0 plays wave 0 with a random pitch effect, whose key region ends at key 64, and whose
// second velocity region plays wave 1 above velocity 63. Its envelope rises over `attack` units, then falls to silence
// over `decay` units and holds there. The release takes 30 units. The key region's high key is the top byte of a word.
inline std::vector<uint8_t> BankV1File(uint32_t number, int16_t attack, int16_t decay, bool big_endian = true)
{
    Bytes b(big_endian);
    b.Tag("IBNK").U32(0).U32(number).U32(1).PadTo(0x20);

    b.Tag("ENVT").U32(30);
    b.U16(0).U16(static_cast<uint16_t>(attack)).U16(0x7fff);
    b.U16(0).U16(static_cast<uint16_t>(decay)).U16(0).U16(14).U16(0).U16(0);
    b.U16(0).U16(30).U16(0).U16(15).U16(0).U16(0); // the release, at 18
    b.PadTo((b.Size() + 3) & ~std::size_t{3});

    b.Tag("OSCT").U32(4 + 0x1c).U32(1);
    b.Tag("Osci").U8(0).U8(0).U16(0).F32(1.0f).U32(0).U32(18).F32(1.0f).F32(0.0f);
    const std::size_t rand = b.Size() + 8;
    b.Tag("RAND").U32(16).Tag("Rand").U8(1).U8(0).U16(0).F32(1.0f).F32(0.02f);

    const std::size_t inst = b.Size() + 8;
    b.Tag("INST").U32(0); // size set below
    b.Tag("Inst").U32(1).U32(0).U32(1).U32(static_cast<uint32_t>(rand)).U32(1);
    b.U32(64u << 24).U32(2);
    b.U8(63).U8(0).U16(0).U32(0).F32(1.0f).F32(1.0f);
    b.U8(127).U8(0).U16(0).U32(1).F32(1.0f).F32(1.0f);
    b.F32(1.0f).F32(1.0f);
    b.Put32(inst - 4, static_cast<uint32_t>(b.Size() - inst));

    b.PadTo((b.Size() + 3) & ~std::size_t{3});
    b.Tag("LIST").U32(8).U32(1).U32(static_cast<uint32_t>(inst));
    b.Put32(4, static_cast<uint32_t>(b.Size()));

    return b.Data();
}

// The bank file of Pikmin's JAudio (.bx) with the wave systems `systems` and the banks `banks`, each in its table's
// order. An empty block leaves an empty place in its table: an entry of size 0. The header and the tables follow the
// file's byte order. The banks come after the tables, then the wave systems, each on a 32-byte boundary, as in the
// game's file. A little-endian file takes the blocks of the Switch's release of Pikmin (WaveSystemFile and BankV0File
// with `offset_bytes` 8).
inline std::vector<uint8_t> BxFile(const std::vector<std::vector<uint8_t>>& systems,
                                   const std::vector<std::vector<uint8_t>>& banks, bool big_endian = true)
{
    constexpr std::size_t kSystemTable = 0x10;
    const std::size_t bank_table = kSystemTable + 8 * systems.size();
    Bytes b(big_endian);
    b.U32(kSystemTable).U32(static_cast<uint32_t>(systems.size()));
    b.U32(static_cast<uint32_t>(bank_table)).U32(static_cast<uint32_t>(banks.size()));
    b.PadTo(bank_table + 8 * banks.size());

    const auto add = [&b](std::size_t entry, const std::vector<uint8_t>& block)
    {
        b.PadTo((b.Size() + 31) & ~std::size_t{31});
        if (!block.empty())
        {
            b.Put32(entry, static_cast<uint32_t>(b.Size()));
            b.Put32(entry + 4, static_cast<uint32_t>(block.size()));
            b.Append(block);
        }
    };

    for (std::size_t i = 0; i < banks.size(); i++)
    {
        add(bank_table + 8 * i, banks[i]);
    }
    for (std::size_t i = 0; i < systems.size(); i++)
    {
        add(kSystemTable + 8 * i, systems[i]);
    }

    return b.Data();
}

// A sound of a sound table: its type byte, its volume in 127ths, the file of the sequence archive that type 0x11 (or
// 0x60 to 0x6f) plays, and its name in the name table.
struct SoundSpec
{
    uint8_t type = 0x10;
    uint8_t volume = 127;
    uint16_t file = 0;
    std::string name;
    uint16_t switches = 0; // the halfword at +6 of an entry in the later layout
};

// A group of a sound table's section, with its name in the name table.
struct GroupSpec
{
    std::string name;
    std::vector<SoundSpec> sounds;
};

// A section of a sound table, with its name in the name table.
struct SectionSpec
{
    std::string name;
    std::vector<GroupSpec> groups;
};

// The blocks of a sound table: the table (BST) and its names (BSTN). Their offsets count from each block's start.
struct SoundTableBlocks
{
    std::vector<uint8_t> table;
    std::vector<uint8_t> names;
};

// A sound table of `sections`, and its name table. Each sound's entry is 12 bytes. A type with bit 0x40 set has the
// later layout: the volume at +1, the sequence archive's file at +2 and the switches at +6. Any other type has the
// earlier one: the volume at +2 and the file in the low half of the word at +8. With `first_layouts`, every entry has
// the first layouts: the volume at +8 and no file. A group gives each sound's type in a byte and its entry's offset in
// the 24 bits after it. Each file number is a 16-bit field.
inline SoundTableBlocks SoundTable(const std::vector<SectionSpec>& sections, bool first_layouts = false,
                                   bool big_endian = true)
{
    // The table: the root lists the sections, each section its groups, each group its sounds.
    Bytes t(big_endian);
    t.Tag("BST ").U32(0).U32(0).U32(0x10); // the root at 0x10
    const std::size_t root = t.Size();
    t.U32(static_cast<uint32_t>(sections.size()));
    for (std::size_t s = 0; s < sections.size(); s++)
    {
        t.U32(0);
    }

    for (std::size_t s = 0; s < sections.size(); s++)
    {
        t.Put32(root + 4 + 4 * s, static_cast<uint32_t>(t.Size()));
        const std::size_t section = t.Size();
        t.U32(static_cast<uint32_t>(sections[s].groups.size()));
        for (std::size_t g = 0; g < sections[s].groups.size(); g++)
        {
            t.U32(0);
        }

        for (std::size_t g = 0; g < sections[s].groups.size(); g++)
        {
            const std::vector<SoundSpec>& sounds = sections[s].groups[g].sounds;
            t.Put32(section + 4 + 4 * g, static_cast<uint32_t>(t.Size()));
            const std::size_t group = t.Size();
            t.U32(static_cast<uint32_t>(sounds.size())).U32(0);
            for (std::size_t i = 0; i < sounds.size(); i++)
            {
                t.U32(0);
            }

            for (std::size_t i = 0; i < sounds.size(); i++)
            {
                t.Put8(group + 8 + 4 * i, sounds[i].type);
                t.Put24(group + 9 + 4 * i, static_cast<uint32_t>(t.Size()));
                if (first_layouts)
                {
                    t.U32(0).U32(0).U8(sounds[i].volume).U8(0).U16(0);
                }
                else if (sounds[i].type & 0x40)
                {
                    t.U8(0).U8(sounds[i].volume).U16(sounds[i].file).U16(0).U16(sounds[i].switches).U32(0);
                }
                else
                {
                    t.U8(0).U8(0).U8(sounds[i].volume).U8(0).U32(0).U16(0).U16(sounds[i].file);
                }
            }
        }
    }

    // The names: the same tree, with a name before each section's and group's list. Strings follow the tree.
    Bytes n(big_endian);
    std::vector<std::pair<std::size_t, std::string>> strings; // where each string's offset goes, and the string
    n.Tag("BSTN").U32(0).U32(0).U32(0x10);
    const std::size_t names_root = n.Size();
    n.U32(static_cast<uint32_t>(sections.size()));
    for (std::size_t s = 0; s < sections.size(); s++)
    {
        n.U32(0);
    }

    for (std::size_t s = 0; s < sections.size(); s++)
    {
        n.Put32(names_root + 4 + 4 * s, static_cast<uint32_t>(n.Size()));
        const std::size_t section = n.Size();
        n.U32(static_cast<uint32_t>(sections[s].groups.size())).U32(0);
        strings.emplace_back(section + 4, sections[s].name);
        for (std::size_t g = 0; g < sections[s].groups.size(); g++)
        {
            n.U32(0);
        }

        for (std::size_t g = 0; g < sections[s].groups.size(); g++)
        {
            const GroupSpec& group = sections[s].groups[g];
            n.Put32(section + 8 + 4 * g, static_cast<uint32_t>(n.Size()));
            const std::size_t names_group = n.Size();
            n.U32(static_cast<uint32_t>(group.sounds.size())).U32(0);
            strings.emplace_back(names_group + 4, group.name);
            for (std::size_t i = 0; i < group.sounds.size(); i++)
            {
                strings.emplace_back(n.Size(), group.sounds[i].name);
                n.U32(0);
            }
        }
    }

    for (const auto& [at, text] : strings)
    {
        n.Put32(at, static_cast<uint32_t>(n.Size()));
        for (const char c : text)
        {
            n.U8(static_cast<uint8_t>(c));
        }
        n.U8(0);
    }

    n.PadTo((n.Size() + 3) & ~std::size_t{3});

    return {t.Data(), n.Data()};
}

// A start-up archive with one wave system (number 0) of two waves, `bank` played with it, a sequence collection of one
// category that holds `sequences`, and the sound table `sounds` if it has a table. A little-endian archive takes a
// little-endian bank and table.
inline std::vector<uint8_t> StartUpArchive(const std::vector<uint8_t>& bank,
                                           const std::vector<std::vector<uint8_t>>& sequences,
                                           const SoundTableBlocks& sounds = {}, bool big_endian = true)
{
    const std::vector<uint8_t> ws = WaveSystemFile(0, 2, "test.aw", big_endian);
    Bytes sc(big_endian);
    sc.U8('S').U8('C').U16(1).U32(0).U32(12);
    sc.U32(static_cast<uint32_t>(sequences.size()));
    std::size_t at = 12 + 4 + 4 * sequences.size();
    for (const std::vector<uint8_t>& s : sequences)
    {
        sc.U32(static_cast<uint32_t>(at));
        at += s.size();
    }

    for (const std::vector<uint8_t>& s : sequences)
    {
        sc.Append(s);
    }

    sc.Put32(4, static_cast<uint32_t>(sc.Size()));

    // The command list, then the blocks.
    const bool table = !sounds.table.empty();
    Bytes a(big_endian);
    a.Tag("AA_<");
    const std::size_t list_size = 4 + 16 + 12 + 12 + (table ? 24 : 0) + 4;
    const auto ws_at = static_cast<uint32_t>(list_size);
    const auto bank_at = static_cast<uint32_t>(ws_at + ws.size());
    const auto sc_at = static_cast<uint32_t>(bank_at + bank.size());
    const auto bst_at = static_cast<uint32_t>(sc_at + sc.Size());
    const auto bstn_at = static_cast<uint32_t>(bst_at + sounds.table.size());

    a.Tag("ws  ").U32(0).U32(ws_at).U32(0xffffffff);
    a.Tag("bnk ").U32(0).U32(bank_at);
    a.Tag("bsc ").U32(sc_at).U32(static_cast<uint32_t>(sc_at + sc.Size()));
    if (table)
    {
        a.Tag("bst ").U32(bst_at).U32(static_cast<uint32_t>(bst_at + sounds.table.size()));
        a.Tag("bstn").U32(bstn_at).U32(static_cast<uint32_t>(bstn_at + sounds.names.size()));
    }

    a.Tag(">_AA");
    a.Append(ws).Append(bank).Append(sc.Data());
    if (table)
    {
        a.Append(sounds.table).Append(sounds.names);
    }

    return a.Data();
}

// The AW file of WaveSystemFile: wave i is 64 samples of a square wave of period 16 at i * 128 bytes. The samples are
// big-endian in either byte order of the other files.
inline std::vector<uint8_t> AwFile()
{
    Bytes aw;
    for (int wave = 0; wave < 2; wave++)
    {
        for (int i = 0; i < 64; i++)
        {
            aw.U16(static_cast<uint16_t>((i / 8) % 2 ? 0x4000 : 0xc000));
        }
    }

    return aw.Data();
}

// `data` compressed with Yaz0, every byte as a literal: groups of eight chunks after a flag byte of 0xff.
inline std::vector<uint8_t> LiteralYaz0(const std::vector<uint8_t>& data)
{
    Bytes yaz0;
    yaz0.Tag("Yaz0").U32(static_cast<uint32_t>(data.size())).U32(0).U32(0);
    for (std::size_t i = 0; i < data.size(); i++)
    {
        if (i % 8 == 0)
        {
            yaz0.U8(0xff);
        }
        yaz0.U8(data[i]);
    }

    return yaz0.Data();
}

// A file of an RARC archive: its id, its name and its data.
struct RarcFileSpec
{
    uint16_t id = 0;
    std::string name;
    std::vector<uint8_t> data;
};

// An RARC archive of `files`, after one folder entry. The info block starts at 0x20. The block's offsets and the data's
// offsets count from there. An entry's kind and its name's offset are one word: the kind in the top byte, and the
// offset in the low half. Each entry takes `entry_size` bytes: 20, or 24 in the wide entries of the Switch's release of
// Pikmin 2. A wide entry ends with 4 more zero bytes.
inline std::vector<uint8_t> RarcFile(const std::vector<RarcFileSpec>& files, bool big_endian = true,
                                     std::size_t entry_size = 20)
{
    // The strings: the folder's name, then the name of each file.
    std::vector<uint8_t> strings = {'.', 0};
    std::vector<uint16_t> name_offsets;
    for (const RarcFileSpec& f : files)
    {
        name_offsets.push_back(static_cast<uint16_t>(strings.size()));
        strings.insert(strings.end(), f.name.begin(), f.name.end());
        strings.push_back(0);
    }

    const std::size_t kEntryTable = 0x40;             // from 0x20
    const std::size_t entry_count = files.size() + 1; // the folder, then the files
    const std::size_t strings_at = kEntryTable + entry_size * entry_count;
    const std::size_t data_at = (strings_at + strings.size() + 31) & ~std::size_t{31};

    Bytes r(big_endian);
    r.Tag("RARC").U32(0).U32(0x20).U32(static_cast<uint32_t>(data_at)).PadTo(0x28);
    r.U32(static_cast<uint32_t>(entry_count)).U32(static_cast<uint32_t>(kEntryTable)).U32(0);
    r.U32(static_cast<uint32_t>(strings_at)).PadTo(0x20 + kEntryTable);
    r.U16(0xffff).U16(0).U32(0x02u << 24).U32(0).U32(0).U32(0); // the folder
    r.PadTo(0x20 + kEntryTable + entry_size);

    std::size_t at = 0;
    for (std::size_t i = 0; i < files.size(); i++)
    {
        r.U16(files[i].id).U16(0).U32(0x01u << 24 | name_offsets[i]);
        r.U32(static_cast<uint32_t>(at)).U32(static_cast<uint32_t>(files[i].data.size())).U32(0);
        r.PadTo(0x20 + kEntryTable + entry_size * (i + 2));
        at += files[i].data.size();
    }

    r.Append(strings).PadTo(0x20 + data_at);
    for (const RarcFileSpec& f : files)
    {
        r.Append(f.data);
    }

    r.Put32(4, static_cast<uint32_t>(r.Size()));

    return r.Data();
}

// A sound of an AAF's sound table: its category (0 to 15 for an effect, 16 for a sequence), its volume, a sequence's
// file in the sequence archive, and its pitch. The entry holds the pitch only if it isn't 1.
struct AafSoundSpec
{
    uint32_t category = 0;
    uint8_t volume = 127;
    uint16_t file = 0;
    float pitch = 1.0f;
};

// A sequence of a sequence index (BarcFile): its name, and its offset and size in the file of sequences.
struct AafSequenceSpec
{
    std::string name;
    uint32_t offset = 0;
    uint32_t size = 0;
};

// A sequence index (BARC) of `sequences`, in the order given, for the file of sequences `archive_name`. "----" follows
// the tag. Each entry holds the sequence's name cut to 13 characters, 0xffff (it shares no other sequence's data), the
// words 2 and 3, then the sequence's offset and size. The word at +8 takes `word_bytes` bytes: 4, or 8 in a
// little-endian index of the Switch's release of Pikmin.
inline std::vector<uint8_t> BarcFile(const std::vector<AafSequenceSpec>& sequences,
                                     const std::string& archive_name = "test.arc", bool big_endian = true,
                                     std::size_t word_bytes = 4)
{
    Bytes b(big_endian);

    // `text`, then zeros up to `size` bytes.
    const auto text = [&b](const std::string& s, std::size_t size)
    {
        const std::size_t at = b.Size();
        for (const char c : s)
        {
            b.U8(static_cast<uint8_t>(c));
        }

        b.PadTo(at + size);
    };

    text("BARC----", 8);
    b.Offset(0, word_bytes).U32(static_cast<uint32_t>(sequences.size()));
    text(archive_name.substr(0, 15), 16);
    for (const AafSequenceSpec& sequence : sequences)
    {
        text(sequence.name.substr(0, 13), 14);
        b.U16(0xffff).U32(2).U32(3).U32(sequence.offset).U32(sequence.size);
    }

    return b.Data();
}

// JAudio1's start-up archive (AAF) with a sound table of `sounds`, one bank, and a wave system (WaveSystemFile) of one
// wave for each name in `archives`. The bank is `bank`, after the other blocks. Without one, the bank is only its tag.
// Wave system n has the id system_ids[n], or n without one. The bank's value in the list is `bank_system`, the wave
// system that the bank plays. The systems' load types alternate: 1 (at start-up),
// then 2 (for a scene). The table lists the sounds of each category in the order given. The sequences' entries come
// first. The Wind Waker's table has them first too. The table's header gives the effects and the streams format 6, and
// the sequences `sequence_format`. The Wind Waker's sequences have format 5, whose bit 0 makes an entry give the file.
// Luigi's Mansion's have format 0. The archive also has a chunk of a type that the sound library skips. With `index`,
// it has a sequence index (BarcFile, chunk type 4) of those sequences, as Luigi's Mansion's has.
//
// A little-endian archive has the layout of the Switch's release of Pikmin 2. Each list entry takes 16 bytes: a 64-bit
// offset, the size and the value. A block of type 5 comes after the sound table's, in 16 bytes too. The wave systems
// have wide records of their archives.
inline std::vector<uint8_t> AafFile(const std::vector<AafSoundSpec>& sounds, const std::vector<std::string>& archives,
                                    uint8_t sequence_format = 5, const std::vector<AafSequenceSpec>& index = {},
                                    bool big_endian = true, const std::vector<uint8_t>& bank = {},
                                    const std::vector<uint32_t>& system_ids = {}, uint32_t bank_system = 0)
{
    std::vector<uint8_t> b;
    const auto put32 = [&b, big_endian](std::size_t at, uint32_t value)
    {
        b.resize(std::max(b.size(), at + 4));
        big_endian ? xsflib::StoreBe32(b.data() + at, value) : xsflib::StoreLe32(b.data() + at, value);
    };
    const auto put16 = [&b, big_endian](std::size_t at, uint32_t value)
    {
        b.resize(std::max(b.size(), at + 2));
        big_endian ? xsflib::StoreBe16(b.data() + at, static_cast<uint16_t>(value))
                   : xsflib::StoreLe16(b.data() + at, static_cast<uint16_t>(value));
    };

    // The chunks: the sound table, the bank list, the wave system list, a chunk that the sound library skips up to its
    // 0 word, the sequence index if there is one, and the end. The index follows the wave systems.
    constexpr std::size_t kTable = 0x100;
    constexpr std::size_t kBank = 0x700;
    constexpr std::size_t kSystems = 0x800;
    constexpr std::size_t kSystemSize = 0x200;
    const std::size_t index_at = kSystems + archives.size() * kSystemSize;
    const std::size_t index_size = 0x20 + 0x20 * index.size();
    const std::vector<uint8_t> barc =
        index.empty() ? std::vector<uint8_t>() : BarcFile(index, "JaiArcS.arc", big_endian);
    const std::size_t bank_at = (index_at + barc.size() + 0x1f) & ~std::size_t{0x1f};

    // A list entry, or the block of type 5: the offset, the size and the value. A little-endian archive widens the
    // offset to 64 bits.
    std::vector<uint32_t> chunks = {1, kTable, 0x600, 0};
    const auto block = [&chunks, big_endian](uint32_t offset, uint32_t size, uint32_t value)
    {
        chunks.insert(chunks.end(), {offset, size, value});
        if (!big_endian)
        {
            chunks.insert(chunks.end() - 2, 0); // the offset's high half
        }
    };

    if (!big_endian)
    {
        chunks.push_back(5);
        block(kTable, 0x10, 0);
    }
    chunks.push_back(2);
    if (bank.empty())
    {
        block(kBank, 0x10, 0);
    }
    else
    {
        block(static_cast<uint32_t>(bank_at), static_cast<uint32_t>(bank.size()), bank_system);
    }
    chunks.insert(chunks.end(), {0, 3});
    for (std::size_t s = 0; s < archives.size(); s++)
    {
        block(static_cast<uint32_t>(kSystems + s * kSystemSize), kSystemSize, s % 2 == 0 ? 1u : 2u);
    }
    chunks.insert(chunks.end(), {0, 0x10, 5, 6, 0});
    if (!index.empty())
    {
        chunks.insert(chunks.end(), {4, static_cast<uint32_t>(index_at), static_cast<uint32_t>(index_size), 0});
    }
    chunks.push_back(0);

    for (std::size_t i = 0; i < chunks.size(); i++)
    {
        put32(i * 4, chunks[i]);
    }

    // The table's header gives the formats of the effects, the sequences and the streams, then each category's count
    // and first entry. Entries are 16 bytes from +0x50. An entry has a sequence's file at +6 and the volume at +0xc.
    // A pitch other than 1 goes at +8. Without one, +8 holds 0, and the reader takes a pitch of 1.
    b.resize(std::max(b.size(), kTable + 3));
    b[kTable] = 6;
    b[kTable + 1] = sequence_format;
    b[kTable + 2] = 6;
    std::size_t entry = 0;
    for (const uint32_t category : {16u, 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u})
    {
        uint32_t count = 0;
        put16(kTable + 8 + 4 * category, static_cast<uint32_t>(entry));
        for (const AafSoundSpec& sound : sounds)
        {
            if (sound.category == category)
            {
                const std::size_t at = kTable + 0x50 + entry++ * 0x10;
                put16(at + 6, sound.file);
                if (sound.pitch != 1.0f)
                {
                    put32(at + 8, std::bit_cast<uint32_t>(sound.pitch));
                }
                b.resize(std::max(b.size(), at + 0x10));
                b[at + 0xc] = sound.volume;
                count++;
            }
        }
        put16(kTable + 6 + 4 * category, count);
    }

    put32(kBank, 0x49424e4b); // "IBNK"

    for (std::size_t s = 0; s < archives.size(); s++)
    {
        const uint32_t id = s < system_ids.size() ? system_ids[s] : static_cast<uint32_t>(s);
        const std::vector<uint8_t> system = WaveSystemFile(id, 1, archives[s], big_endian, big_endian ? 0x70 : 0x8c);
        const std::size_t at = kSystems + s * kSystemSize;
        b.resize(std::max(b.size(), at + system.size()));
        std::copy(system.begin(), system.end(), b.begin() + static_cast<std::ptrdiff_t>(at));
    }

    b.resize(std::max(b.size(), index_at));
    if (!index.empty())
    {
        b.resize(index_at);
        b.insert(b.end(), barc.begin(), barc.end());
    }
    if (!bank.empty())
    {
        b.resize(bank_at);
        b.insert(b.end(), bank.begin(), bank.end());
    }

    return b;
}

// A sequence that opens track 0 at 0x20, and the track there, which plays key 60 for a beat on program 0 of bank 0.
inline std::vector<uint8_t> OneNoteSequence()
{
    Bytes s;
    s.U8(0xc1).U8(0).U24(0x20);  // open track 0
    s.U8(0xd8).U8(0x62).U16(48); // a timebase of 48
    s.U8(0xe0).U16(120);         // a tempo of 120
    s.U8(0xf1).U8(200).U8(0xff); // wait, then finish
    while (s.Size() < 0x20)
    {
        s.U8(0xfe);
    }

    s.U8(0xe2).U8(0).U8(0xe3).U8(0); // bank 0, program 0
    s.U8(60).U8(1).U8(100);          // key 60 on note 1, at a velocity of 100
    s.U8(0xf1).U8(48).U8(0x81);      // a beat, then note 1's end
    s.U8(0xff);

    return s.Data();
}

// Write the sound files of a small JAudio2 game in `folder`, as a disc holds them: the start-up archive test.baa, its
// sequence archive Seqs/test.arc and its wave archive Waves/test.aw. The sound table holds one piece of music,
// BGM_FIELD, which plays OneNoteSequence. Throw std::runtime_error if a file can't be written.
inline void WriteGameFiles(const std::filesystem::path& folder)
{
    const SoundTableBlocks sounds = SoundTable({{"BGM", {{"FIELD", {{0x11, 127, 7, "BGM_FIELD"}}}}}});
    const std::vector<std::pair<std::filesystem::path, std::vector<uint8_t>>> game = {
        {"test.baa", StartUpArchive(BankV0File(0, 60), {}, sounds)},
        {std::filesystem::path("Seqs") / "test.arc", RarcFile({{7, "field.bms", OneNoteSequence()}})},
        {std::filesystem::path("Waves") / "test.aw", AwFile()}};
    for (const auto& [path, bytes] : game)
    {
        std::filesystem::create_directories((folder / path).parent_path());
        if (const std::optional<std::string> error = xsflib::WriteFile(folder / path, bytes))
        {
            throw std::runtime_error(*error);
        }
    }
}

} // namespace ntdlib::testing::jaudio_files
