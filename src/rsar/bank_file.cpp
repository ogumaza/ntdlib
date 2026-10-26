// SPDX-License-Identifier: MIT

#include "ntdlib/rsar/bank_file.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "rsar/file_blocks.h"
#include "rsar/wave_infos.h"
#include "sar/file_bytes.h"

namespace ntdlib::rsar
{
namespace
{

constexpr std::size_t kReferenceSize = 8;
constexpr std::size_t kRegionSize = 0x30;
constexpr std::size_t kEmptyNodeSize = 4;
constexpr std::size_t kIndexTableHeader = 4;
constexpr std::size_t kMaxDepth = 256;
constexpr std::size_t kMostRangeEntries = 11;
constexpr uint16_t kOldVersion = 0x0101;
constexpr uint16_t kNewVersion = 0x0102;
constexpr int kTop = 127;

using detail::BlockOf;
using detail::CloseBlock;
using detail::CloseFile;
using detail::Header;
using detail::kBlockHeader;
using detail::OpenBlock;
using detail::VersionOf;
using sar::detail::Out;
using TreeReference = BankFile::Reference;
using TreeNode = BankFile::Node;

xsflib::FormatError OtherLayout(const std::string& what)
{
    return xsflib::FormatError("a bank that the tools didn't lay out: " + what);
}

// Where the references of a range table that holds `count` entries start, from the table's start.
std::size_t RangeReferences(std::size_t count)
{
    return (count + 1 + 3) & ~std::size_t{3};
}

// The bytes that `node` takes in the layout.
std::size_t SizeOf(const TreeNode& node)
{
    switch (node.kind)
    {
    case BankFile::kRegion:
        return kRegionSize;
    case BankFile::kRangeTable:
        return RangeReferences(node.keys.size()) + node.entries.size() * kReferenceSize;
    case BankFile::kIndexTable:
        return kIndexTableHeader + node.entries.size() * kReferenceSize;
    default:
        return kEmptyNodeSize;
    }
}

void PutRegion(Out& out, std::size_t at, const Region& region)
{
    out.U32(at, static_cast<uint32_t>(region.wave_index));
    out.U8(at + 4, region.attack);
    out.U8(at + 5, region.decay);
    out.U8(at + 6, region.sustain);
    out.U8(at + 7, region.release);
    out.U8(at + 8, region.hold);
    out.U8(at + 9, region.wave_data_location);
    out.U8(at + 0xA, region.note_off_type);
    out.U8(at + 0xB, region.key_group);
    out.U8(at + 0xC, region.original_key);
    out.U8(at + 0xD, region.volume);
    out.U8(at + 0xE, region.pan);
    out.U8(at + 0xF, region.surround_pan);
    out.U32(at + 0x10, std::bit_cast<uint32_t>(region.tune));
}

Region ReadRegion(const xsflib::Reader& r, std::size_t at)
{
    Region region;
    region.wave_index = r.S32(at);
    region.attack = r.U8(at + 4);
    region.decay = r.U8(at + 5);
    region.sustain = r.U8(at + 6);
    region.release = r.U8(at + 7);
    region.hold = r.U8(at + 8);
    region.wave_data_location = r.U8(at + 9);
    region.note_off_type = r.U8(at + 0xA);
    region.key_group = r.U8(at + 0xB);
    region.original_key = r.U8(at + 0xC);
    region.volume = r.U8(at + 0xD);
    region.pan = r.U8(at + 0xE);
    region.surround_pan = r.U8(at + 0xF);
    region.tune = r.F32(at + 0x10);

    return region;
}

// The reader of a bank's tree, in the order of the layout.
class TreeReader
{
public:
    TreeReader(const xsflib::Reader& r, std::size_t base, std::vector<TreeNode>& nodes)
        : r_(r), base_(base), nodes_(nodes)
    {
    }

    // Notes the kind of each node that a reference of its data type names, from the reference at `ref_at` on. A
    // reference of data type 4 names a node of any kind, and gives no kind.
    void Gather(std::size_t ref_at, std::size_t depth)
    {
        const uint8_t type = r_.U8(ref_at + 1);
        const uint32_t offset = r_.U32(ref_at + 4);
        if (r_.U8(ref_at) != 1 || type < BankFile::kRegion || type > BankFile::kIndexTable)
        {
            return;
        }
        if (depth > kMaxDepth)
        {
            throw OtherLayout("a tree deeper than " + std::to_string(kMaxDepth) + " nodes");
        }

        const auto [known, added] = kinds_.emplace(offset, type);
        if (!added)
        {
            if (known->second != type)
            {
                throw OtherLayout("references of two data types name one node");
            }
            return;
        }
        for (const std::size_t entry : EntriesAt(type, base_ + offset))
        {
            Gather(entry, depth + 1);
        }
    }

    // The reference at `ref_at`. A node goes into the layout where a reference first names it, before the nodes that
    // its entries name.
    TreeReference Visit(std::size_t ref_at, std::size_t depth)
    {
        const uint8_t kind = r_.U8(ref_at);
        const uint8_t type = r_.U8(ref_at + 1);
        const uint32_t offset = r_.U32(ref_at + 4);
        if (kind == 0 && type == 0 && offset == 0)
        {
            return {};
        }
        if (kind != 1 || type < BankFile::kRegion || type > BankFile::kOtherNode)
        {
            throw OtherLayout("a reference of another kind");
        }
        if (const auto known = indices_.find(offset); known != indices_.end())
        {
            return {type, known->second};
        }
        if (depth > kMaxDepth)
        {
            throw OtherLayout("a tree deeper than " + std::to_string(kMaxDepth) + " nodes");
        }

        const auto index = static_cast<uint32_t>(nodes_.size());
        indices_.emplace(offset, index);
        TreeNode node;
        if (type != BankFile::kOtherNode)
        {
            node.kind = type;
        }
        else
        {
            const auto known = kinds_.find(offset);
            node.kind = known != kinds_.end() ? known->second : BankFile::kNone;
        }

        const std::size_t at = base_ + offset;
        if (node.kind == BankFile::kRegion)
        {
            node.region = ReadRegion(r_, at);
        }
        else if (node.kind == BankFile::kRangeTable)
        {
            const uint8_t count = r_.U8(at);
            for (std::size_t i = 0; i < count; ++i)
            {
                node.keys.push_back(r_.U8(at + 1 + i));
            }
        }
        else if (node.kind == BankFile::kIndexTable)
        {
            node.keys = {r_.U8(at), r_.U8(at + 1)};
            if (node.keys[1] < node.keys[0])
            {
                throw OtherLayout("an index table whose keys run backwards");
            }
        }
        nodes_.push_back(std::move(node));

        for (const std::size_t entry : EntriesAt(nodes_[index].kind, at))
        {
            const TreeReference child = Visit(entry, depth + 1);
            nodes_[index].entries.push_back(child);
        }

        return {type, index};
    }

private:
    // Where the references of a node of `kind` at `at` lie.
    std::vector<std::size_t> EntriesAt(uint8_t kind, std::size_t at) const
    {
        std::vector<std::size_t> entries;
        if (kind == BankFile::kRangeTable)
        {
            const uint8_t count = r_.U8(at);
            for (std::size_t i = 0; i < count; ++i)
            {
                entries.push_back(at + RangeReferences(count) + i * kReferenceSize);
            }
        }
        else if (kind == BankFile::kIndexTable)
        {
            const uint8_t lowest = r_.U8(at);
            const uint8_t highest = r_.U8(at + 1);
            for (int key = lowest; key <= highest; ++key)
            {
                entries.push_back(at + kIndexTableHeader + static_cast<std::size_t>(key - lowest) * kReferenceSize);
            }
        }

        return entries;
    }

    const xsflib::Reader& r_;
    std::size_t base_ = 0;
    std::vector<TreeNode>& nodes_;
    std::map<uint32_t, uint8_t> kinds_;
    std::map<uint32_t, uint32_t> indices_;
};

// The keys or velocities that each entry of the table `node` covers, as NW4R SND chooses an entry
// (BankFileReader::GetReferenceToSubRegion): an index table's entry covers its key, and a range table's entry the keys
// up to its key that no earlier entry covers. An entry that covers nothing gives an empty run.
std::vector<std::pair<int, int>> CoveredRuns(const TreeNode& node)
{
    std::vector<std::pair<int, int>> runs;
    int first = node.kind == BankFile::kIndexTable ? node.keys[0] : 0;
    for (std::size_t i = 0; i < node.entries.size(); ++i)
    {
        const int last = node.kind == BankFile::kIndexTable ? node.keys[0] + static_cast<int>(i) : node.keys[i];
        runs.emplace_back(first, std::min(last, kTop));
        first = std::max(first, last + 1);
    }

    return runs;
}

// Checks that `runs` follow one another without overlaps, each from its first to its last, up to 127.
void CheckRuns(const std::vector<std::pair<int, int>>& runs, const char* what)
{
    int previous = -1;
    for (const auto& [first, last] : runs)
    {
        if (first > last || last > kTop || first <= previous)
        {
            throw std::invalid_argument(std::string("an instrument's ") + what +
                                        " overlap, run backwards, don't follow their order or pass 127");
        }
        previous = last;
    }
}

// The builder of FromInstruments' tree, in the order of the layout.
class TreeBuilder
{
public:
    explicit TreeBuilder(std::vector<TreeNode>& nodes) : nodes_(nodes)
    {
    }

    TreeReference Instrument(const BankFile::Instrument& instrument)
    {
        std::vector<std::pair<int, int>> runs;
        for (const BankFile::KeyRegion& key : instrument.key_regions)
        {
            runs.emplace_back(key.first_key, key.last_key);
        }
        CheckRuns(runs, "key regions");
        if (runs.empty())
        {
            return {};
        }
        if (runs.size() == 1 && runs[0] == std::pair{0, kTop} && Whole(instrument.key_regions[0]))
        {
            return Velocities(instrument.key_regions[0]);
        }

        return Table(runs, [&](std::size_t k) { return Velocities(instrument.key_regions[k]); });
    }

private:
    // Whether `key` holds one velocity region over every velocity.
    static bool Whole(const BankFile::KeyRegion& key)
    {
        return key.regions.size() == 1 && key.regions[0].first_velocity == 0 && key.regions[0].last_velocity == kTop;
    }

    TreeReference Region(const rsar::Region& values)
    {
        const auto index = static_cast<uint32_t>(nodes_.size());
        TreeNode& node = nodes_.emplace_back();
        node.kind = BankFile::kRegion;
        node.region = values;

        return {BankFile::kRegion, index};
    }

    // A key region's node: its region, or a table of its velocity regions.
    TreeReference Velocities(const BankFile::KeyRegion& key)
    {
        std::vector<std::pair<int, int>> runs;
        for (const BankFile::VelocityRegion& velocity : key.regions)
        {
            runs.emplace_back(velocity.first_velocity, velocity.last_velocity);
        }
        CheckRuns(runs, "velocity regions");
        if (runs.empty())
        {
            return {};
        }
        if (Whole(key))
        {
            return Region(key.regions[0].values);
        }

        return Table(runs, [&](std::size_t v) { return Region(key.regions[v].values); });
    }

    // A table of `runs`, whose entries `make` lays out in order.
    template <typename Make>
    TreeReference Table(const std::vector<std::pair<int, int>>& runs, const Make& make)
    {
        // A range table's entries: each gap before or between the runs, each run, and the gap after the last.
        std::vector<std::pair<int, std::optional<std::size_t>>> entries;
        int end = -1;
        for (std::size_t i = 0; i < runs.size(); ++i)
        {
            if (runs[i].first > end + 1)
            {
                entries.emplace_back(runs[i].first - 1, std::nullopt);
            }
            entries.emplace_back(runs[i].second, i);
            end = runs[i].second;
        }
        if (end < kTop)
        {
            entries.emplace_back(kTop, std::nullopt);
        }

        const auto index = static_cast<uint32_t>(nodes_.size());
        nodes_.emplace_back();
        if (entries.size() <= kMostRangeEntries)
        {
            nodes_[index].kind = BankFile::kRangeTable;
            for (const auto& [last, run] : entries)
            {
                const TreeReference entry = run ? make(*run) : TreeReference{};
                nodes_[index].keys.push_back(static_cast<uint8_t>(last));
                nodes_[index].entries.push_back(entry);
            }

            return {BankFile::kRangeTable, index};
        }

        // An index table from the first run's first key to the last run's last. Each key of a run names the run's node.
        nodes_[index].kind = BankFile::kIndexTable;
        nodes_[index].keys = {static_cast<uint8_t>(runs.front().first), static_cast<uint8_t>(runs.back().second)};
        std::size_t run = 0;
        TreeReference current;
        for (int key = runs.front().first; key <= runs.back().second; ++key)
        {
            if (key == runs[run].first)
            {
                current = make(run);
            }
            const bool inside = key >= runs[run].first && key <= runs[run].second;
            nodes_[index].entries.push_back(inside ? current : TreeReference{});
            if (key == runs[run].second && run + 1 < runs.size())
            {
                ++run;
            }
        }

        return {BankFile::kIndexTable, index};
    }

    std::vector<TreeNode>& nodes_;
};

// Checks that Read gives the tree of `bank` back. A node of a table's kind or a region has to lie where a reference
// first names it in pre-order, and a reference of its kind has to reach it through references of their kinds. Any
// other node lies where references of data type 4 first name it, and has kind kNone.
void CheckTree(const BankFile& bank)
{
    const std::vector<TreeNode>& nodes = bank.nodes;
    const auto check = [&](const TreeReference& reference)
    {
        if (reference.type > BankFile::kOtherNode || (reference.type != BankFile::kNone && reference.node >= nodes.size()))
        {
            throw std::invalid_argument("a reference of an unknown data type, or to a node that the bank lacks");
        }
        if (reference.type != BankFile::kNone && reference.type != BankFile::kOtherNode &&
            reference.type != nodes[reference.node].kind)
        {
            throw std::invalid_argument("a reference's data type doesn't match its node's kind");
        }
    };

    // The kinds that references of their data types give, as Read gathers them.
    std::vector<uint8_t> kinds(nodes.size(), BankFile::kNone);
    const auto gather = [&](const auto& self, const TreeReference& reference, std::size_t depth) -> void
    {
        check(reference);
        if (reference.type == BankFile::kNone || reference.type == BankFile::kOtherNode ||
            kinds[reference.node] != BankFile::kNone)
        {
            return;
        }
        if (depth > kMaxDepth)
        {
            throw std::invalid_argument("a tree deeper than " + std::to_string(kMaxDepth) + " nodes");
        }
        kinds[reference.node] = reference.type;
        for (const TreeReference& entry : nodes[reference.node].entries)
        {
            self(self, entry, depth + 1);
        }
    };

    // The order in which references first name the nodes, as Read lays them out.
    std::vector<bool> placed(nodes.size());
    uint32_t next = 0;
    const auto visit = [&](const auto& self, const TreeReference& reference, std::size_t depth) -> void
    {
        check(reference);
        if (reference.type == BankFile::kNone || placed[reference.node])
        {
            return;
        }
        if (reference.node != next)
        {
            throw std::invalid_argument("the nodes don't follow the order in which references first name them");
        }
        if (depth > kMaxDepth)
        {
            throw std::invalid_argument("a tree deeper than " + std::to_string(kMaxDepth) + " nodes");
        }
        placed[reference.node] = true;
        ++next;
        const TreeNode& node = nodes[reference.node];
        if (node.kind != kinds[reference.node])
        {
            throw std::invalid_argument("a node that only references of data type 4 reach has a kind");
        }
        if ((node.kind == BankFile::kRegion || node.kind == BankFile::kNone) &&
            (!node.keys.empty() || !node.entries.empty()))
        {
            throw std::invalid_argument("a region or a node of kind kNone has keys or entries");
        }
        for (const TreeReference& entry : node.entries)
        {
            self(self, entry, depth + 1);
        }
    };

    for (const TreeReference& instrument : bank.instruments)
    {
        gather(gather, instrument, 0);
    }
    for (const TreeReference& instrument : bank.instruments)
    {
        visit(visit, instrument, 0);
    }
    if (next != nodes.size())
    {
        throw std::invalid_argument("a node that no reference names");
    }
}

} // namespace

BankFile BankFile::Read(std::span<const uint8_t> file, std::span<const uint8_t> wave_data)
{
    const xsflib::Reader r(file, true);
    BankFile bank;
    bank.version = VersionOf(r, "RBNK");
    if (bank.version != kOldVersion && bank.version != kNewVersion)
    {
        throw xsflib::FormatError("an RBNK file of a version other than 1.1 and 1.2");
    }

    // The tree.
    const auto [data, data_size] = BlockOf(r, 0, "DATA", "RBNK");
    const std::size_t base = data + kBlockHeader;
    const uint32_t count = r.U32(base);
    if (count > file.size() / kReferenceSize)
    {
        throw xsflib::FormatError("an RBNK file lists more programs than it can hold");
    }
    TreeReader tree(r, base, bank.nodes);
    for (uint32_t p = 0; p < count; ++p)
    {
        tree.Gather(base + 4 + p * kReferenceSize, 0);
    }
    for (uint32_t p = 0; p < count; ++p)
    {
        bank.instruments.push_back(tree.Visit(base + 4 + p * kReferenceSize, 0));
    }

    // The waves: in WAVE over the raw samples, or in the RWAR.
    if (bank.version == kOldVersion)
    {
        const auto [wave, wave_size] = BlockOf(r, 1, "WAVE", "RBNK");
        const std::size_t payload = wave + kBlockHeader;
        const uint32_t waves = r.U32(payload);
        if (waves > file.size() / kReferenceSize)
        {
            throw xsflib::FormatError("an RBNK file lists more waves than it can hold");
        }
        std::vector<std::size_t> infos;
        for (uint32_t i = 0; i < waves; ++i)
        {
            infos.push_back(payload + r.U32(payload + 4 + i * kReferenceSize + 4));
        }
        bank.waves = detail::ReadRawWaves(r, infos, wave_data);
    }
    else
    {
        for (const std::vector<uint8_t>& rwav : WaveArchiveFile::Read(wave_data).waves)
        {
            bank.waves.push_back(WaveFile::Read(rwav));
        }
    }

    Files written;
    try
    {
        written = bank.Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw OtherLayout(error.what());
    }
    if (!std::ranges::equal(written.file, file))
    {
        throw OtherLayout("Write gives other bytes of the file");
    }
    if (!std::ranges::equal(written.wave_data, wave_data))
    {
        throw OtherLayout("Write gives other bytes of the wave data");
    }

    return bank;
}

BankFile::Files BankFile::Write() const
{
    if (version != kOldVersion && version != kNewVersion)
    {
        throw std::invalid_argument("a bank of a version other than 1.1 and 1.2");
    }

    CheckTree(*this);

    // Each node's offset from the number of programs, in the order of the nodes.
    std::vector<uint32_t> offsets;
    std::size_t end = 4 + instruments.size() * kReferenceSize;
    for (const Node& node : nodes)
    {
        offsets.push_back(static_cast<uint32_t>(end));
        end += SizeOf(node);
    }

    Out out = Header("RBNK", version, version == kOldVersion ? 2 : 1);
    const std::size_t data = OpenBlock(out, "DATA");
    const std::size_t base = out.Zeros(end);
    out.U32(base, static_cast<uint32_t>(instruments.size()));
    const auto put = [&](std::size_t at, const Reference& reference)
    {
        if (reference.type == kNone)
        {
            return;
        }
        out.U8(at, 1);
        out.U8(at + 1, reference.type);
        out.U32(at + 4, offsets[reference.node]);
    };
    for (std::size_t p = 0; p < instruments.size(); ++p)
    {
        put(base + 4 + p * kReferenceSize, instruments[p]);
    }
    for (std::size_t n = 0; n < nodes.size(); ++n)
    {
        const Node& node = nodes[n];
        const std::size_t at = base + offsets[n];
        if (node.kind == kRegion)
        {
            PutRegion(out, at, node.region);
        }
        else if (node.kind == kRangeTable)
        {
            if (node.keys.size() != node.entries.size() || node.keys.size() > 0xFF)
            {
                throw std::invalid_argument("a range table's keys don't match its entries");
            }
            out.U8(at, static_cast<uint8_t>(node.keys.size()));
            for (std::size_t i = 0; i < node.keys.size(); ++i)
            {
                out.U8(at + 1 + i, node.keys[i]);
            }
            for (std::size_t i = 0; i < node.entries.size(); ++i)
            {
                put(at + RangeReferences(node.keys.size()) + i * kReferenceSize, node.entries[i]);
            }
        }
        else if (node.kind == kIndexTable)
        {
            if (node.keys.size() != 2 || node.keys[1] < node.keys[0] ||
                node.entries.size() != std::size_t{node.keys[1]} - node.keys[0] + 1)
            {
                throw std::invalid_argument("an index table's keys run backwards or don't match its entries");
            }
            out.U8(at, node.keys[0]);
            out.U8(at + 1, node.keys[1]);
            for (std::size_t i = 0; i < node.entries.size(); ++i)
            {
                put(at + kIndexTableHeader + i * kReferenceSize, node.entries[i]);
            }
        }
        else if (node.kind != kNone)
        {
            throw std::invalid_argument("a node of an unknown kind");
        }
    }
    CloseBlock(out, data, 0);

    Files files;
    if (version == kOldVersion)
    {
        const detail::RawWaves raw = detail::LayRawWaves(waves);
        const std::size_t wave = OpenBlock(out, "WAVE");
        const std::size_t payload = out.Zeros(4 + waves.size() * kReferenceSize);
        out.U32(payload, static_cast<uint32_t>(waves.size()));
        for (std::size_t i = 0; i < waves.size(); ++i)
        {
            const std::size_t info = out.Zeros(detail::WaveInfoSize(waves[i].encoding, waves[i].channels.size()));
            out.Reference(payload + 4 + i * kReferenceSize, 0x0100, static_cast<uint32_t>(info - payload));
            detail::PutWaveInfo(out, info, waves[i], raw.places[i], raw.channel_offsets[i]);
        }
        CloseBlock(out, wave, 1);
        files.wave_data = raw.wave_data;
    }
    else
    {
        WaveArchiveFile archive;
        for (const WaveFile& wave : waves)
        {
            archive.waves.push_back(wave.Write());
        }
        files.wave_data = archive.Write();
    }
    files.file = CloseFile(out);

    return files;
}

std::vector<std::optional<BankFile::Instrument>> BankFile::Instruments() const
{
    // The velocity regions that a key region's node plays: a region over every velocity, or each entry of a table
    // that names a region. Adjacent velocities of an index table that name one region make one velocity region.
    const auto velocities = [this](const Reference& reference)
    {
        std::vector<VelocityRegion> regions;
        if (reference.type == kRegion)
        {
            regions.push_back({0, kTop, nodes[reference.node].region});
        }
        if (reference.type != kRangeTable && reference.type != kIndexTable)
        {
            return regions;
        }

        const Node& table = nodes[reference.node];
        const std::vector<std::pair<int, int>> runs = CoveredRuns(table);
        for (std::size_t i = 0; i < runs.size(); ++i)
        {
            const auto [first, last] = runs[i];
            const Reference& entry = table.entries[i];
            if (entry.type != kRegion || first > last)
            {
                continue;
            }
            if (table.kind == kIndexTable && i > 0 && table.entries[i - 1] == entry)
            {
                regions.back().last_velocity = static_cast<uint8_t>(last);
                continue;
            }
            regions.push_back({static_cast<uint8_t>(first), static_cast<uint8_t>(last), nodes[entry.node].region});
        }

        return regions;
    };

    std::vector<std::optional<Instrument>> result;
    for (const Reference& reference : instruments)
    {
        Instrument instrument;
        if (reference.type == kRegion)
        {
            instrument.key_regions.push_back({0, kTop, velocities(reference)});
        }
        else if (reference.type == kRangeTable || reference.type == kIndexTable)
        {
            const Node& table = nodes[reference.node];
            const std::vector<std::pair<int, int>> runs = CoveredRuns(table);
            for (std::size_t i = 0; i < runs.size(); ++i)
            {
                const auto [first, last] = runs[i];
                const Reference& entry = table.entries[i];
                std::vector<VelocityRegion> regions = velocities(entry);
                if (regions.empty() || first > last)
                {
                    continue;
                }
                if (table.kind == kIndexTable && i > 0 && table.entries[i - 1] == entry)
                {
                    instrument.key_regions.back().last_key = static_cast<uint8_t>(last);
                    continue;
                }
                instrument.key_regions.push_back(
                    {static_cast<uint8_t>(first), static_cast<uint8_t>(last), std::move(regions)});
            }
        }

        if (instrument.key_regions.empty())
        {
            result.emplace_back();
        }
        else
        {
            result.push_back(std::move(instrument));
        }
    }

    return result;
}

BankFile BankFile::FromInstruments(uint16_t version, std::span<const std::optional<Instrument>> instruments,
                                   std::vector<WaveFile> waves)
{
    BankFile bank;
    bank.version = version;
    bank.waves = std::move(waves);
    TreeBuilder builder(bank.nodes);
    for (const std::optional<Instrument>& instrument : instruments)
    {
        bank.instruments.push_back(instrument ? builder.Instrument(*instrument) : Reference{});
    }

    return bank;
}

} // namespace ntdlib::rsar
