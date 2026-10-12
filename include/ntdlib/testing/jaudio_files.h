// SPDX-License-Identifier: MIT

// Builds small JAudio2 files for tests, in the layout of Four Swords Adventures' files: wave systems (WSYS) with their
// AW file, banks (IBNK) of both versions, sound tables with their names (BST and BSTN), sequence archives (RARC) and
// start-up archives (BAA). It also builds JAudio1's start-up archive (AAF), in The Wind Waker's layout. The GameCube's
// formats are big-endian.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

namespace ntdlib::testing::jaudio_files
{

// Big-endian data under construction.
class Bytes
{
public:
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
        return U8(v >> 8).U8(v);
    }

    Bytes& U24(uint32_t v)
    {
        return U8(v >> 16).U16(v);
    }

    Bytes& U32(uint32_t v)
    {
        return U16(v >> 16).U16(v);
    }

    Bytes& F32(float v)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, 4);
        return U32(bits);
    }

    Bytes& Tag(const char* tag)
    {
        for (int i = 0; i < 4; i++)
        {
            U8(static_cast<uint8_t>(tag[i]));
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

    void Put32(std::size_t at, uint32_t v)
    {
        xsflib::StoreBe32(data_.data() + at, v);
    }

    const std::vector<uint8_t>& Data() const
    {
        return data_;
    }

private:
    std::vector<uint8_t> data_;
};

// A wave system with one wave archive, `archive_name`, of `waves` PCM16 waves of 64 samples each at 32000 Hz, key 60.
// The first loops over its last 32 samples. The archive's name has at most 111 characters.
inline std::vector<uint8_t> WaveSystemFile(uint32_t id, int waves, const std::string& archive_name = "test.aw")
{
    Bytes w;
    w.Tag("WSYS").U32(0).U32(id).U32(0).U32(0x20).U32(0); // size, id, table size, WINF, WBCT (set below)
    w.PadTo(0x20);

    const std::size_t winf = w.Size();
    w.Tag("WINF").U32(1).U32(0); // one archive
    const std::size_t archive = w.Size();
    w.Put32(winf + 8, static_cast<uint32_t>(archive));

    std::vector<uint8_t> name(0x70);
    std::copy_n(archive_name.begin(), std::min<std::size_t>(archive_name.size(), 0x6f), name.begin());
    w.Append(name).U32(static_cast<uint32_t>(waves));

    const std::size_t offsets = w.Size();
    for (int i = 0; i < waves; i++)
    {
        w.U32(0);
    }

    for (int i = 0; i < waves; i++)
    {
        w.Put32(offsets + 4 * static_cast<std::size_t>(i), static_cast<uint32_t>(w.Size()));
        const bool loop = i == 0;
        w.U8(0).U8(3).U8(60).U8(0).F32(32000.0f);
        w.U32(static_cast<uint32_t>(128 * i)).U32(128); // offset and size in the AW file
        w.U32(loop ? 0xffffffff : 0).U32(loop ? 32 : 0).U32(loop ? 64 : 0).U32(64);
        w.U16(0).U16(0);
    }

    // WBCT: one scene whose C-DF gives wave i the id i.
    const std::size_t wbct = w.Size();
    w.Put32(0x14, static_cast<uint32_t>(wbct));
    w.Tag("WBCT").U32(0).U32(1).U32(static_cast<uint32_t>(wbct + 16));
    const std::size_t scene = w.Size();
    w.Tag("SCNE").U32(0).U32(0).U32(static_cast<uint32_t>(scene + 16));
    w.Tag("C-DF").U32(static_cast<uint32_t>(waves));

    const std::size_t entries = w.Size();
    for (int i = 0; i < waves; i++)
    {
        w.U32(0);
    }

    for (int i = 0; i < waves; i++)
    {
        w.Put32(entries + 4 * static_cast<std::size_t>(i), static_cast<uint32_t>(w.Size()));
        w.U32(static_cast<uint32_t>(id << 16 | static_cast<uint32_t>(i)));
    }

    w.Put32(4, static_cast<uint32_t>(w.Size()));

    return w.Data();
}

// A version 0 bank whose instrument 0 plays wave 0 on every key, with an instant attack, a hold and a release of
// `release` units. Drum set 0 (program 0xe4) plays wave 1 on key 36, panned right.
inline std::vector<uint8_t> BankV0File(uint32_t number, int16_t release)
{
    Bytes b;
    b.Tag("IBNK").U32(0).U32(number).U32(0).PadTo(0x20);
    b.Tag("BANK").PadTo(0x3b4 + 12 * 4);

    // Oscillator tables.
    const std::size_t attack = b.Size();
    b.U16(0).U16(0).U16(0x7fff).U16(14).U16(0).U16(0);
    const std::size_t release_table = b.Size();
    b.U16(0).U16(static_cast<uint16_t>(release)).U16(0).U16(15).U16(0).U16(0);
    const std::size_t osc = b.Size();
    b.U8(0).U8(0).U16(0).F32(1.0f).U32(static_cast<uint32_t>(attack)).U32(static_cast<uint32_t>(release_table));
    b.F32(1.0f).F32(0.0f);

    // A velocity region, a key region and the instrument.
    const std::size_t vmap = b.Size();
    b.U8(127).U8(0).U16(0).U32(0).F32(1.0f).F32(1.0f);
    const std::size_t keymap = b.Size();
    b.U8(127).U8(0).U16(0).U32(1).U32(static_cast<uint32_t>(vmap));
    const std::size_t inst = b.Size();
    b.Tag("INST").U32(0).F32(1.0f).F32(1.0f).U32(static_cast<uint32_t>(osc)).U32(0);
    b.U32(0).U32(0).U32(0).U32(0).U32(1).U32(static_cast<uint32_t>(keymap));
    b.Put32(0x24, static_cast<uint32_t>(inst));

    // The drum set.
    const std::size_t drum_vmap = b.Size();
    b.U8(127).U8(0).U16(0).U32(1).F32(0.5f).F32(1.0f);
    const std::size_t pmap = b.Size();
    b.F32(1.0f).F32(2.0f).U32(0).U32(0).U32(1).U32(static_cast<uint32_t>(drum_vmap));
    const std::size_t perc = b.Size();
    b.Tag("PER2").PadTo(perc + 0x88 + 4 * 128 + 128 + 256);
    b.Put32(perc + 0x88 + 4 * 36, static_cast<uint32_t>(pmap));

    std::vector<uint8_t> d = b.Data();
    d[perc + 0x288 + 36] = 127;   // pan
    d[perc + 0x308 + 2 * 36] = 0; // release: 200 units
    d[perc + 0x308 + 2 * 36 + 1] = 200;

    Bytes out;
    out.Append(d);
    out.Put32(0x3b4, static_cast<uint32_t>(perc));
    out.Put32(4, static_cast<uint32_t>(out.Size()));

    return out.Data();
}

// A version 1 bank whose program 0 plays wave 0 with a random pitch effect, whose key region ends at key 64, and whose
// second velocity region plays wave 1 above velocity 63. Its envelope rises over `attack` units, then falls to silence
// over `decay` units and holds there. The release takes 30 units.
inline std::vector<uint8_t> BankV1File(uint32_t number, int16_t attack, int16_t decay)
{
    Bytes b;
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
    const std::size_t inst_start = b.Size();
    b.Tag("Inst").U32(1).U32(0).U32(1).U32(static_cast<uint32_t>(rand)).U32(1);
    b.U8(64).U8(0).U16(0).U32(2);
    b.U8(63).U8(0).U16(0).U32(0).F32(1.0f).F32(1.0f);
    b.U8(127).U8(0).U16(0).U32(1).F32(1.0f).F32(1.0f);
    b.F32(1.0f).F32(1.0f);

    std::vector<uint8_t> d = b.Data();
    const auto inst_size = static_cast<uint32_t>(d.size() - inst_start);
    Bytes out;
    out.Append(d);
    out.Put32(inst - 4, inst_size);
    out.PadTo((out.Size() + 3) & ~std::size_t{3});
    out.Tag("LIST").U32(8).U32(1).U32(static_cast<uint32_t>(inst));
    out.Put32(4, static_cast<uint32_t>(out.Size()));

    return out.Data();
}

// A sound of a sound table: its type byte, its volume in 127ths, the file of the sequence archive that type 0x11 (or
// 0x60 to 0x6f) plays, and its name in the name table.
struct SoundSpec
{
    uint8_t type = 0x10;
    uint8_t volume = 127;
    uint16_t file = 0;
    std::string name;
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
// later layout: the volume at +1 and the sequence archive's file at +2. Any other type has the earlier one: the volume
// at +2 and the file at +8.
inline SoundTableBlocks SoundTable(const std::vector<SectionSpec>& sections)
{
    // The table: the root lists the sections, each section its groups, each group its sounds.
    Bytes t;
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
                t.Put32(group + 8 + 4 * i, uint32_t{sounds[i].type} << 24 | static_cast<uint32_t>(t.Size()));
                if (sounds[i].type & 0x40)
                {
                    t.U8(0).U8(sounds[i].volume).U16(sounds[i].file).U32(0).U32(0);
                }
                else
                {
                    t.U8(0).U8(0).U8(sounds[i].volume).U8(0).U32(0).U32(sounds[i].file);
                }
            }
        }
    }

    // The names: the same tree, with a name before each section's and group's list. Strings follow the tree.
    Bytes n;
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
// category that holds `sequences`, and the sound table `sounds` if it has a table.
inline std::vector<uint8_t> StartUpArchive(const std::vector<uint8_t>& bank,
                                           const std::vector<std::vector<uint8_t>>& sequences,
                                           const SoundTableBlocks& sounds = {})
{
    const std::vector<uint8_t> ws = WaveSystemFile(0, 2);
    Bytes sc;
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
    Bytes a;
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

// The AW file of WaveSystemFile: wave i is 64 samples of a square wave of period 16 at i * 128 bytes.
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

// A file of an RARC archive: its id, its name and its data.
struct RarcFileSpec
{
    uint16_t id = 0;
    std::string name;
    std::vector<uint8_t> data;
};

// An RARC archive of `files`, after one folder entry. The info block starts at 0x20. The block's offsets and the data's
// offsets count from there.
inline std::vector<uint8_t> RarcFile(const std::vector<RarcFileSpec>& files)
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
    const std::size_t strings_at = kEntryTable + 20 * entry_count;
    const std::size_t data_at = (strings_at + strings.size() + 31) & ~std::size_t{31};

    Bytes r;
    r.Tag("RARC").U32(0).U32(0x20).U32(static_cast<uint32_t>(data_at)).PadTo(0x28);
    r.U32(static_cast<uint32_t>(entry_count)).U32(static_cast<uint32_t>(kEntryTable)).U32(0);
    r.U32(static_cast<uint32_t>(strings_at)).PadTo(0x20 + kEntryTable);
    r.U16(0xffff).U16(0).U8(0x02).U8(0).U16(0).U32(0).U32(0).U32(0); // the folder

    std::size_t at = 0;
    for (std::size_t i = 0; i < files.size(); i++)
    {
        r.U16(files[i].id).U16(0).U8(0x01).U8(0).U16(name_offsets[i]);
        r.U32(static_cast<uint32_t>(at)).U32(static_cast<uint32_t>(files[i].data.size())).U32(0);
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

// A sound of an AAF's sound table: its category (0 to 15 for an effect, 16 for a sequence), its volume, and a
// sequence's file in the sequence archive.
struct AafSoundSpec
{
    uint32_t category = 0;
    uint8_t volume = 127;
    uint16_t file = 0;
};

// JAudio1's start-up archive (AAF) with a sound table of `sounds`, one bank, and a wave system (WaveSystemFile) of one
// wave for each name in `archives`. Wave system n has the id n. The systems' load types alternate: 1 (at start-up),
// then 2 (for a scene). The table lists the sounds of each category in the order given. The sequences' entries come
// first. The Wind Waker's table has them first too. The archive also has a chunk of a type that the sound library
// skips.
inline std::vector<uint8_t> AafFile(const std::vector<AafSoundSpec>& sounds, const std::vector<std::string>& archives)
{
    std::vector<uint8_t> b;
    const auto put32 = [&b](std::size_t at, uint32_t value)
    {
        b.resize(std::max(b.size(), at + 4));
        xsflib::StoreBe32(b.data() + at, value);
    };
    const auto put16 = [&b](std::size_t at, uint32_t value)
    {
        b.resize(std::max(b.size(), at + 2));
        b[at] = static_cast<uint8_t>(value >> 8);
        b[at + 1] = static_cast<uint8_t>(value);
    };

    // The chunks: the sound table, the bank list, the wave system list, a chunk that the sound library skips up to its
    // 0 word, and the end.
    constexpr std::size_t kTable = 0x100;
    constexpr std::size_t kBank = 0x700;
    constexpr std::size_t kSystems = 0x800;
    constexpr std::size_t kSystemSize = 0x200;
    std::vector<uint32_t> chunks = {1, kTable, 0x600, 0, 2, kBank, 0x10, 0, 0, 3};
    for (std::size_t s = 0; s < archives.size(); s++)
    {
        chunks.insert(chunks.end(),
                      {static_cast<uint32_t>(kSystems + s * kSystemSize), kSystemSize, s % 2 == 0 ? 1u : 2u});
    }
    chunks.insert(chunks.end(), {0, 0x10, 5, 6, 0, 0});

    for (std::size_t i = 0; i < chunks.size(); i++)
    {
        put32(i * 4, chunks[i]);
    }

    // The table's header gives each category its count and its first entry. Entries are 16 bytes from +0x50. An entry
    // has a sequence's file at +6 and the volume at +0xc.
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
        const std::vector<uint8_t> system = WaveSystemFile(static_cast<uint32_t>(s), 1, archives[s]);
        const std::size_t at = kSystems + s * kSystemSize;
        b.resize(std::max(b.size(), at + system.size()));
        std::copy(system.begin(), system.end(), b.begin() + static_cast<std::ptrdiff_t>(at));
    }

    b.resize(std::max(b.size(), kSystems + archives.size() * kSystemSize));

    return b;
}

} // namespace ntdlib::testing::jaudio_files
