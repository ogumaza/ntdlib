// SPDX-License-Identifier: MIT

// Tests of the readers of JAudio1's start-up archive (jaudio/aaf.h), with a made-up archive in either byte order.

#include "ntdlib/jaudio/aaf.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "jaudio_describe.h"
#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/testing/jaudio_files.h"

namespace jaudio = ntdlib::jaudio;
namespace files = ntdlib::testing::jaudio_files;
using xsflib::testing::ErrorOf;

namespace
{

using ntdlib::testing::jaudio_describe::Describe;

// The effects sequence (the table's first sequence) in file 7, two pieces of music in files 3 and 9, and effects in
// categories 0 and 5.
const std::vector<files::AafSoundSpec> kSounds = {
    {16, 127, 7}, {16, 100, 3}, {16, 90, 9}, {0, 80}, {0, 81}, {5, 82},
};

void TestSounds()
{
    const std::vector<jaudio::SoundEntry> sounds = jaudio::ReadAafSounds(files::AafFile(kSounds, {"a_0.aw"}));

    XSFLIB_CHECK(sounds.size() == 5);
    if (sounds.size() != 5)
    {
        return;
    }

    // The effects, by category and index, then the music without the effects sequence. An effect's ID has bit 11. The
    // sound library then plays the effect to its end.
    XSFLIB_CHECK(sounds[0].Id() == 0x0800 && sounds[0].volume == 80 && sounds[0].type == 0);
    XSFLIB_CHECK(sounds[1].Id() == 0x0801 && sounds[1].volume == 81);
    XSFLIB_CHECK(sounds[2].Id() == 0x5800 && jaudio::AafCategory(sounds[2].Id()) == 5);
    XSFLIB_CHECK(sounds[0].source == jaudio::SoundEntry::Source::kCollection && sounds[0].resource == 0);
    XSFLIB_CHECK(sounds[3].Id() == 0x80000001 && sounds[3].volume == 100 && sounds[3].type == 0x80);

    // An effect's resource is its number in the effects sequence. The effects' format, 6, makes that its index.
    XSFLIB_CHECK(sounds[1].resource == 1 && sounds[2].resource == 0);
    XSFLIB_CHECK(sounds[3].source == jaudio::SoundEntry::Source::kArchive && sounds[3].resource == 3);
    XSFLIB_CHECK(sounds[4].Id() == 0x80000002 && sounds[4].resource == 9);
    XSFLIB_CHECK(sounds[0].name.empty() && sounds[4].name.empty());

    // An entry's offset: the table at 0x100, its entries from +0x50. The effects of category 0 follow the three
    // sequences.
    XSFLIB_CHECK(sounds[0].offset == 0x100 + 0x50 + 3 * 0x10);
}

// The message with which ReadAafSounds rejects the archive, or an empty one.
std::string Refusal(std::span<const uint8_t> aaf)
{
    return ErrorOf([&] { jaudio::ReadAafSounds(aaf); });
}

void TestDamaged()
{
    // An archive cut short, an archive without a sound table, and a table without sounds are rejected.
    const std::vector<uint8_t> aaf = files::AafFile(kSounds, {});
    XSFLIB_CHECK(Refusal(aaf).empty());
    XSFLIB_CHECK(Refusal(std::span(aaf).first(0x180)) == "the AAF archive is damaged");

    std::vector<uint8_t> no_table = aaf;
    no_table[3] = 4; // the sound table's chunk becomes a chunk of another type
    XSFLIB_CHECK(Refusal(no_table) == "the AAF archive has no sound table");
    XSFLIB_CHECK(Refusal(files::AafFile({{16, 127, 7}}, {})) == "the AAF archive's sound table lists no sounds");
}

void TestEffectsSequence()
{
    XSFLIB_CHECK(jaudio::AafEffectsSequenceFile(files::AafFile(kSounds, {})) == 7);
}

void TestSequenceFormat()
{
    // Without bit 0 of the sequences' format, a sequence's file is its index, whatever its entry holds there. The
    // effects sequence is index 0.
    const std::vector<uint8_t> aaf = files::AafFile(kSounds, {}, 0);
    const std::vector<jaudio::SoundEntry> sounds = jaudio::ReadAafSounds(aaf);

    XSFLIB_CHECK(sounds.size() == 5);
    if (sounds.size() == 5)
    {
        XSFLIB_CHECK(sounds[3].Id() == 0x80000001 && sounds[3].resource == 1);
        XSFLIB_CHECK(sounds[4].Id() == 0x80000002 && sounds[4].resource == 2);
        XSFLIB_CHECK(sounds[0].resource == 0 && sounds[0].source == jaudio::SoundEntry::Source::kCollection);
    }

    XSFLIB_CHECK(jaudio::AafEffectsSequenceFile(aaf) == 0);

    // Any format with bit 0 gives the entry's file.
    XSFLIB_CHECK(jaudio::AafEffectsSequenceFile(files::AafFile(kSounds, {}, 3)) == 7);
}

void TestEffectFormat()
{
    // With bit 0 of the effects' format, an effect's entry gives its number, as a sequence's entry gives its file. The
    // effects' format is the first byte of the table's header, at 0x100.
    std::vector<uint8_t> aaf = files::AafFile({{16, 127, 7}, {0, 80, 12}, {0, 81, 5, 1.5f}}, {});
    aaf[0x100] |= 1;
    const std::vector<jaudio::SoundEntry> sounds = jaudio::ReadAafSounds(aaf);

    XSFLIB_CHECK(sounds.size() == 2 && sounds[0].resource == 12 && sounds[1].resource == 5);

    // With bit 1, as in format 6, an effect plays at the pitch of its entry. A pitch of 0 counts as 1. Without the bit,
    // every effect keeps a pitch of 1.
    XSFLIB_CHECK(sounds.size() == 2 && sounds[0].pitch == 1.0f && sounds[1].pitch == 1.5f);
    aaf[0x100] = 4;
    const std::vector<jaudio::SoundEntry> plain = jaudio::ReadAafSounds(aaf);
    XSFLIB_CHECK(plain.size() == 2 && plain[1].pitch == 1.0f);
}

// The sequences of TestSequenceIndex: their names and where they lie in a 0x100-byte archive.
const std::vector<files::AafSequenceSpec> kIndex = {
    {"se.bms", 0, 0x40}, {"Mansion.com", 0x40, 0x80}, {"VeryLongNameOfAPiece.com", 0xc0, 0x40}};

// The message with which ReadAafSequenceFiles rejects the archive, or an empty one.
std::string IndexRefusal(std::span<const uint8_t> aaf, std::span<const uint8_t> archive)
{
    return ErrorOf([&] { jaudio::ReadAafSequenceFiles(aaf, archive); });
}

void TestSequenceIndex()
{
    // The index's sequences, by their numbers. A name keeps its first 13 characters. The data lies in the archive.
    std::vector<uint8_t> archive(0x100);
    for (std::size_t i = 0; i < archive.size(); i++)
    {
        archive[i] = static_cast<uint8_t>(i);
    }

    const std::vector<uint8_t> aaf = files::AafFile(kSounds, {"a_0.aw"}, 0, kIndex);
    const std::optional<std::vector<jaudio::ArchiveFile>> sequences = jaudio::ReadAafSequenceFiles(aaf, archive);

    XSFLIB_CHECK(sequences && sequences->size() == 3);
    if (sequences && sequences->size() == 3)
    {
        const auto& s = *sequences;
        XSFLIB_CHECK(s[0].id == 0 && s[0].name == "se.bms" && s[0].data.data() == archive.data());
        XSFLIB_CHECK(s[0].data.size() == 0x40);
        XSFLIB_CHECK(s[1].id == 1 && s[1].name == "Mansion.com" && s[1].data.size() == 0x80 && s[1].data[0] == 0x40);
        XSFLIB_CHECK(s[2].id == 2 && s[2].name == "VeryLongNameO" && s[2].data.size() == 0x40 && s[2].data[0] == 0xc0);
        XSFLIB_CHECK(s[0].data_id == 0 && s[1].data_id == 1 && s[2].data_id == 2);
    }

    // A sequence that shares another's data reads that sequence's data in the game. The 16 bits after sequence 2's
    // name name sequence 0.
    std::vector<uint8_t> shared = aaf;
    const std::size_t kShareAt = 0x800 + 0x200 + 0x20 + 2 * 0x20 + 14;
    shared[kShareAt] = 0;
    shared[kShareAt + 1] = 0;
    const std::optional<std::vector<jaudio::ArchiveFile>> sharing = jaudio::ReadAafSequenceFiles(shared, archive);
    XSFLIB_CHECK(sharing && sharing->size() == 3 && (*sharing)[2].data_id == 0 && (*sharing)[2].data[0] == 0xc0);

    // The other readers skip the index.
    XSFLIB_CHECK(jaudio::ReadAafSounds(aaf).size() == 5 && jaudio::ReadAafWaveSystems(aaf).size() == 1);

    // An archive without an index gives nothing.
    XSFLIB_CHECK(!jaudio::ReadAafSequenceFiles(files::AafFile(kSounds, {}), archive));

    // A sequence that ends past the archive, an index that isn't one, and an index cut short are rejected.
    XSFLIB_CHECK(IndexRefusal(aaf, archive).empty());
    XSFLIB_CHECK(IndexRefusal(aaf, std::span(archive).first(0xff)) ==
                 "the AAF archive's sequence index names a sequence outside the sequence archive");

    std::vector<uint8_t> not_index = aaf;
    const std::size_t kIndexAt = 0x800 + 0x200;
    not_index[kIndexAt] = 'X';
    XSFLIB_CHECK(IndexRefusal(not_index, archive) == "the AAF archive's sequence index is damaged");
    XSFLIB_CHECK(IndexRefusal(std::span(aaf).first(kIndexAt + 0x20 + 0x30), archive) == "the AAF archive is damaged");
}

void TestWaveSystems()
{
    // Each system is numbered by its place in the list. Two systems with the same archive name it once. Systems 0 and 2
    // load at start-up, and system 1 for a scene.
    const std::vector<uint8_t> aaf = files::AafFile(kSounds, {"a_0.aw", "b_0.aw", "a_0.aw"});

    const std::vector<jaudio::WaveSystem> systems = jaudio::ReadAafWaveSystems(aaf);

    XSFLIB_CHECK(systems.size() == 3);
    for (std::size_t s = 0; s < systems.size(); s++)
    {
        XSFLIB_CHECK(systems[s].number == s && systems[s].groups.size() == 1);
        XSFLIB_CHECK(!systems[s].groups.empty() && systems[s].groups[0].waves == std::vector<uint16_t>{0});
    }

    XSFLIB_CHECK(jaudio::AafWaveArchiveNames(aaf) == (std::vector<std::string>{"a_0.aw", "b_0.aw"}));
    XSFLIB_CHECK(jaudio::AafStayingWaveArchiveNames(aaf) == (std::vector<std::string>{"a_0.aw"}));
}

void TestIsAaf()
{
    // An AAF starts with the type of a chunk that gives a block, 1 to 8. A BAA starts with "AA_<".
    XSFLIB_CHECK(jaudio::IsAaf(files::AafFile(kSounds, {})));
    XSFLIB_CHECK(!jaudio::IsAaf(std::vector<uint8_t>{'A', 'A', '_', '<', 0, 0, 0, 0}));
    XSFLIB_CHECK(!jaudio::IsAaf(std::vector<uint8_t>{0, 0, 0, 0}) && !jaudio::IsAaf(std::vector<uint8_t>{0, 0, 0, 9}));
    XSFLIB_CHECK(jaudio::IsAaf(std::vector<uint8_t>{0, 0, 0, 8}) && !jaudio::IsAaf(std::vector<uint8_t>{0, 0, 1}));
}

void TestBanks()
{
    // The made-up archive's bank is only its magic. It can't be read, and a warning names its offset.
    std::vector<std::string> warnings;
    const std::vector<jaudio::Bank> banks = jaudio::ReadAafBanks(files::AafFile(kSounds, {"a_0.aw"}), warnings);

    XSFLIB_CHECK(banks.empty());
    XSFLIB_CHECK(warnings.size() == 1 && warnings[0].starts_with("bank at offset 1792: "));
}

// A name file of `sounds`: the 16-byte header with the count, 18 records for the categories, then a record for each
// sound. A record is a name of up to 28 bytes, then the sound's ID. The count is big-endian in either order.
std::vector<uint8_t> NameFile(const std::vector<jaudio::AsnSound>& sounds, bool big_endian = true)
{
    std::vector<uint8_t> b(16 + (18 + sounds.size()) * 32);
    xsflib::StoreBe32(b.data() + 12, static_cast<uint32_t>(sounds.size()));
    for (std::size_t i = 0; i < 18; i++)
    {
        const std::string category = std::to_string(i) + " CATEGORY";
        std::copy(category.begin(), category.end(), b.begin() + static_cast<std::ptrdiff_t>(16 + i * 32));
    }
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const std::size_t at = 16 + (18 + i) * 32;
        std::copy(sounds[i].name.begin(), sounds[i].name.end(), b.begin() + static_cast<std::ptrdiff_t>(at));
        big_endian ? xsflib::StoreBe32(b.data() + at + 28, sounds[i].id)
                   : xsflib::StoreLe32(b.data() + at + 28, sounds[i].id);
    }

    return b;
}

void TestAsnSounds()
{
    // The sounds follow the category records, in the file's order. A name can fill its 28 bytes.
    const std::string kLongest(28, 'N');
    const std::vector<uint8_t> asn =
        NameFile({{0x80000800, "JA_BGM_SE_SEQUENCE"}, {0x80000001, "JA_BGM_FIELD"}, {0x00000805, kLongest}});

    const std::vector<jaudio::AsnSound> sounds = jaudio::ReadAsnSounds(asn);

    XSFLIB_CHECK(sounds.size() == 3);
    if (sounds.size() == 3)
    {
        XSFLIB_CHECK(sounds[0].id == 0x80000800 && sounds[0].name == "JA_BGM_SE_SEQUENCE");
        XSFLIB_CHECK(sounds[1].id == 0x80000001 && sounds[1].name == "JA_BGM_FIELD");
        XSFLIB_CHECK(sounds[2].id == 0x805 && sounds[2].name == kLongest);
    }

    // A file too short for its count is rejected.
    XSFLIB_CHECK(ErrorOf([&] { jaudio::ReadAsnSounds(std::span(asn).first(asn.size() - 1)); }) ==
                 "the name file is too short for its sounds");

    // The Switch's release of Pikmin 2 stores its IDs little-endian. The count stays big-endian.
    const std::vector<uint8_t> le =
        NameFile({{0x80000800, "JA_BGM_SE_SEQUENCE"}, {0x80000001, "JA_BGM_FIELD"}, {0x00000805, kLongest}}, false);
    const std::vector<jaudio::AsnSound> from_le = jaudio::ReadAsnSounds(le, false);
    XSFLIB_CHECK(le != asn && from_le.size() == 3);
    for (std::size_t i = 0; i < std::min(from_le.size(), sounds.size()); i++)
    {
        XSFLIB_CHECK(from_le[i].id == sounds[i].id && from_le[i].name == sounds[i].name);
    }
    XSFLIB_CHECK(jaudio::ReadAsnSounds(le).at(1).id == 0x01000080);
}

// The text of every field that the readers give for a sound or a wave system, to compare what two archives give.
std::string Describe(const jaudio::SoundEntry& s)
{
    return "sound " + std::to_string(s.Id()) + " " + std::to_string(s.type) + " " + std::to_string(s.volume) + " " +
           std::to_string(static_cast<int>(s.source)) + " " + std::to_string(s.resource) + " " +
           std::to_string(std::bit_cast<uint32_t>(s.pitch)) + " " + std::to_string(s.offset) + "\n";
}

// Everything that the readers give for an archive: its sounds, its effects sequence, its wave systems and their
// archives, and the offsets of the banks that can't be read. The made-up archive's bank is only its magic. The reasons
// differ between the byte orders: the reader reads a little-endian bank of version 0 past its size.
std::string Describe(std::span<const uint8_t> aaf)
{
    std::string text;
    for (const jaudio::SoundEntry& sound : jaudio::ReadAafSounds(aaf))
    {
        text += Describe(sound);
    }

    text += "effects sequence " + std::to_string(jaudio::AafEffectsSequenceFile(aaf)) + "\n";
    for (const jaudio::WaveSystem& ws : jaudio::ReadAafWaveSystems(aaf))
    {
        text += Describe(ws);
    }
    for (const std::string& name : jaudio::AafWaveArchiveNames(aaf))
    {
        text += "archive " + name + "\n";
    }
    for (const std::string& name : jaudio::AafStayingWaveArchiveNames(aaf))
    {
        text += "staying archive " + name + "\n";
    }

    std::vector<std::string> warnings;
    text += "banks " + std::to_string(jaudio::ReadAafBanks(aaf, warnings).size()) + "\n";
    for (const std::string& warning : warnings)
    {
        text += "warning " + warning.substr(0, warning.find(':')) + "\n";
    }

    return text;
}

// `aaf` with a table of scenes (type 6) in place of the chunk that the sound library skips. Both take four words. The
// table goes at the archive's end. Scene n's row gives each category of `rows[n]` its count of slots, and a limit of 1.
// A little-endian table widens the scenes' offsets to 64 bits.
std::vector<uint8_t> WithScenes(std::vector<uint8_t> aaf, const std::vector<std::vector<uint8_t>>& rows,
                                bool big_endian = true)
{
    const auto put32 = [big_endian](std::vector<uint8_t>& b, std::size_t at, uint32_t value)
    {
        b.resize(std::max(b.size(), at + 4));
        big_endian ? xsflib::StoreBe32(b.data() + at, value) : xsflib::StoreLe32(b.data() + at, value);
    };

    const std::size_t offset_bytes = big_endian ? 4 : 8;
    std::vector<uint8_t> table;
    put32(table, 0, static_cast<uint32_t>(rows.size()));
    std::size_t row_at = 4 + offset_bytes * rows.size();
    for (std::size_t n = 0; n < rows.size(); n++)
    {
        put32(table, 4 + offset_bytes * n, static_cast<uint32_t>(row_at));
        table.resize(std::max(table.size(), 4 + offset_bytes * (n + 1)));
        row_at += 2 * rows[n].size();
    }

    for (const std::vector<uint8_t>& row : rows)
    {
        for (const uint8_t count : row)
        {
            table.insert(table.end(), {count, 1});
        }
    }

    // The skipped chunk's words: its type 0x10, then 5, 6 and the 0 that ends it.
    std::vector<uint8_t> skipped;
    for (const uint32_t word : {0x10u, 5u, 6u, 0u})
    {
        put32(skipped, skipped.size(), word);
    }

    const auto found = std::ranges::search(aaf, skipped);
    const auto at = static_cast<std::size_t>(found.begin() - aaf.begin());
    put32(aaf, at, 6);
    put32(aaf, at + 4, static_cast<uint32_t>(aaf.size()));
    put32(aaf, at + 8, static_cast<uint32_t>(table.size()));
    put32(aaf, at + 12, 0);
    aaf.insert(aaf.end(), table.begin(), table.end());

    return aaf;
}

// The message with which AafEffectSlotCounts rejects the archive, or an empty one.
std::string SlotRefusal(std::span<const uint8_t> aaf)
{
    return ErrorOf([&] { jaudio::AafEffectSlotCounts(aaf); });
}

void TestEffectSlots()
{
    // Without a table of scenes, the sound library gives each category four slots. The readers report no table.
    const std::vector<uint8_t> plain = files::AafFile(kSounds, {});
    XSFLIB_CHECK(!jaudio::AafEffectSlotCounts(plain));

    // The counts of scene 0, as in Pikmin 2: its row runs to scene 1's. The other readers skip the table.
    const std::vector<std::vector<uint8_t>> kScenes = {{2, 2, 4, 4, 3, 5}, {2, 2, 3, 3, 3, 3}, {2, 2, 3}};
    const std::vector<uint8_t> kScene0 = {2, 2, 4, 4, 3, 5};
    const std::vector<uint8_t> scenes = WithScenes(plain, kScenes);
    XSFLIB_CHECK(jaudio::AafEffectSlotCounts(scenes) == kScene0);
    XSFLIB_CHECK(Describe(scenes) == Describe(plain));

    // A table of one scene: the row runs to the table's end.
    XSFLIB_CHECK(jaudio::AafEffectSlotCounts(WithScenes(plain, {{8, 7, 1}})) == std::vector<uint8_t>({8, 7, 1}));

    // A little-endian archive widens the offsets.
    const std::vector<uint8_t> le = files::AafFile(kSounds, {}, 5, {}, false);
    XSFLIB_CHECK(jaudio::AafEffectSlotCounts(WithScenes(le, kScenes, false)) == kScene0);

    // A table of no scenes, and a row that starts past the table's end, are rejected.
    XSFLIB_CHECK(SlotRefusal(WithScenes(plain, {})) == "the AAF archive's table of scenes lists no scenes");
    std::vector<uint8_t> past = scenes;
    const std::size_t table_at = past.size() - (4 + 4 * kScenes.size()) - 2 * 15;
    past[table_at + 7] = 0xff; // the low byte of scene 0's offset
    XSFLIB_CHECK(SlotRefusal(past) == "the AAF archive is damaged");
}

// A little-endian archive in the layout of the Switch's release of Pikmin 2 gives what the big-endian one gives. Its
// block of type 5 comes before the lists. A reader that took the block as 12 bytes would miss the lists.
void TestLittleEndian()
{
    const std::vector<files::AafSoundSpec> kPitched = {
        {16, 127, 7}, {16, 100, 0x123}, {0, 80}, {0, 81, 0, 1.5f}, {5, 82},
    };
    const std::vector<std::string> kArchives = {"a_0.aw", "b_0.aw"};
    const std::vector<uint8_t> be = files::AafFile(kPitched, kArchives);
    const std::vector<uint8_t> le = files::AafFile(kPitched, kArchives, 5, {}, false);
    const std::string from_be = Describe(be);

    XSFLIB_CHECK(le[0] == 1 && le[3] == 0 && le != be);
    XSFLIB_CHECK(jaudio::IsAaf(le) && jaudio::IsAaf(std::vector<uint8_t>{8, 0, 0, 0}));
    XSFLIB_CHECK(!jaudio::IsAaf(std::vector<uint8_t>{9, 0, 0, 0}) && !jaudio::IsAaf(std::vector<uint8_t>{1, 0, 0, 1}));
    XSFLIB_CHECK(from_be.find("sound 2147483649 128 100 1 291 ") != std::string::npos);
    XSFLIB_CHECK(from_be.find("group b_0.aw 0") != std::string::npos);
    XSFLIB_CHECK(from_be.find("staying archive a_0.aw") != std::string::npos);
    XSFLIB_CHECK(from_be.find("warning bank at offset 1792") != std::string::npos);
    XSFLIB_CHECK(Describe(le) == from_be);
    XSFLIB_CHECK(jaudio::SoundArchive::ParseAaf(le).Sounds().size() == 4);
}

} // namespace

int main()
{
    TestSounds();
    TestDamaged();
    TestEffectsSequence();
    TestEffectSlots();
    TestSequenceFormat();
    TestEffectFormat();
    TestSequenceIndex();
    TestWaveSystems();
    TestIsAaf();
    TestBanks();
    TestAsnSounds();
    TestLittleEndian();

    return xsflib::testing::Summary("aaf");
}
