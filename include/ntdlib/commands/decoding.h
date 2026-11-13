// SPDX-License-Identifier: MIT

// The parts that the consoles' decoders of bytecode share. A decoder follows the commands from the sequences' starts,
// through every jump, call and track start, and keeps the bytes that no command covers as data. A guess may then read
// commands in those bytes too. The decoder then lays the commands and the data out as the items of a Sequence.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

namespace ntdlib::commands::detail
{

// A command that the decoder read, at its offset.
struct Read
{
    Command command;
    uint32_t size = 0;
    bool raw = false; // the encoder would write other bytes: the item keeps the bytes
};

// What covers each byte: nothing yet, a command's first byte, or one of its other bytes.
enum class Cover : uint8_t
{
    kFree,
    kStart,
    kInside,
};

// The commands that the decoder has found, and the bytes they cover.
struct Found
{
    std::map<uint32_t, Read> reads;
    std::vector<Cover> cover;
};

// Whether no command covers the `size` bytes from `at`.
inline bool Free(const Found& found, uint32_t at, uint32_t size)
{
    const auto start = found.cover.begin() + at;
    return std::all_of(start, start + size, [](Cover c) { return c == Cover::kFree; });
}

// Records `read` at `at`, and the bytes that it covers.
inline void Mark(Found& found, uint32_t at, Read read)
{
    found.cover[at] = Cover::kStart;
    std::fill(found.cover.begin() + at + 1, found.cover.begin() + at + read.size, Cover::kInside);
    found.reads.emplace(at, std::move(read));
}

// Follows the commands of `data` from each offset in `pending`, through every jump, call and track start, and records
// them in `found`. `read_at(at, first)` reads the command at `at`, or gives nothing where it runs past the end of
// `data`. `first` is true for the first command from each offset of `pending`. `falls_through(command)` gives whether
// the flow goes on after the command. Returns false where a command overlaps another, or where the flow leads inside a
// command.
template <typename ReadAt, typename FallsThrough>
bool Follow(std::span<const uint8_t> data, std::vector<uint32_t> pending, Found& found, ReadAt read_at,
            FallsThrough falls_through)
{
    while (!pending.empty())
    {
        uint32_t at = pending.back();
        pending.pop_back();
        bool first = true;
        while (at < data.size() && found.cover[at] == Cover::kFree)
        {
            std::optional<Read> read = read_at(at, first);
            first = false;
            if (!read)
            {
                break; // the rest stays data
            }
            if (!Free(found, at, read->size))
            {
                return false;
            }

            if (HasTarget(read->command.kind))
            {
                pending.push_back(read->command.target);
            }

            const bool next = falls_through(read->command);
            const uint32_t size = read->size;
            Mark(found, at, std::move(*read));
            if (!next)
            {
                break;
            }

            at += size;
        }
        if (at < data.size() && found.cover[at] == Cover::kInside)
        {
            return false;
        }
    }

    return true;
}

// Reads the commands in the bytes that the flow never reaches, such as the end after a track's last jump or a track
// that no command starts. A run of them counts from the start of the bytes that no command covers, up to an end, a jump
// or a return, or up to a command that the flow reaches. With `many_runs`, more runs may follow it, such as a
// subroutine that the first one calls, and the most runs whose targets all lead to items count. Without it, only the
// first run counts, where its targets all lead to items. Bytes that no such run covers stay as data. `read_at(at,
// first)` reads the command at `at`, or gives nothing where the runs stop before it. `first` is true for the first
// command of the bytes that no command covers. `falls_through` is the test that Follow takes, and `offset_of` is the
// one that Items takes. A target without an offset leads to an item.
template <typename ReadAt, typename FallsThrough, typename OffsetOf>
void Guess(std::span<const uint8_t> data, const std::set<uint32_t>& cuts, bool many_runs, Found& found, ReadAt read_at,
           FallsThrough falls_through, OffsetOf offset_of)
{
    const auto size = static_cast<uint32_t>(data.size());
    uint32_t at = 0;
    while (at < size)
    {
        if (found.cover[at] == Cover::kStart)
        {
            at += found.reads.at(at).size;
            continue;
        }

        // The bytes that no command covers, up to the next command or cut.
        uint32_t end = at + 1;
        while (end < size && found.cover[end] == Cover::kFree && !cuts.contains(end))
        {
            end++;
        }

        // Runs of commands one after another, each up to a command that ends it.
        std::vector<std::pair<uint32_t, Read>> run;
        std::vector<std::size_t> run_ends; // the commands up to each run's end
        uint32_t next = at;
        while (next < end)
        {
            std::optional<Read> read = read_at(next, next == at);
            if (!read || read->raw || next + read->size > end)
            {
                break;
            }

            const uint32_t read_size = read->size;
            const bool counts = !falls_through(read->command) ||
                                (next + read_size == end && end < size && found.cover[end] == Cover::kStart);
            run.emplace_back(next, std::move(*read));
            next += read_size;
            if (counts)
            {
                run_ends.push_back(run.size());
                if (!many_runs)
                {
                    break;
                }
            }
        }

        // A target leads to a command, a cut, a place outside the bytes, or a command of the runs taken.
        std::size_t taken = 0;
        for (auto count = run_ends.rbegin(); count != run_ends.rend() && taken == 0; ++count)
        {
            const auto first = run.begin();
            const auto last = run.begin() + static_cast<std::ptrdiff_t>(*count);
            const auto leads = [&](uint32_t target)
            {
                const std::optional<uint32_t> offset = offset_of(target);
                if (!offset || *offset >= size)
                {
                    return true;
                }
                return found.cover[*offset] == Cover::kStart || cuts.contains(*offset) ||
                       std::any_of(first, last, [&](const auto& read) { return read.first == *offset; });
            };
            const auto target_leads = [&](const auto& read)
            {
                const Command& command = read.second.command;
                return !HasTarget(command.kind) || leads(command.target);
            };
            if (std::all_of(first, last, target_leads))
            {
                taken = *count;
            }
        }
        if (taken == 0)
        {
            at = end;
            continue;
        }

        at = run[taken - 1].first + run[taken - 1].second.size;
        for (std::size_t i = 0; i < taken; i++)
        {
            Mark(found, run[i].first, std::move(run[i].second));
        }
    }
}

// The offsets of the labels in `labels` and of `entries`, where items start. Throws std::invalid_argument for one past
// the end of bytecode of `size` bytes.
template <typename Label>
std::set<uint32_t> Starts(std::size_t size, std::span<const Label> labels, std::span<const uint32_t> entries)
{
    std::set<uint32_t> starts;
    for (const Label& label : labels)
    {
        if (label.offset > size)
        {
            throw std::invalid_argument("a label past the end of the sequence: " + label.name);
        }

        starts.insert(label.offset);
    }
    for (const uint32_t entry : entries)
    {
        if (entry > size)
        {
            throw std::invalid_argument("an entry past the end of the sequence: " + std::to_string(entry));
        }

        starts.insert(entry);
    }

    return starts;
}

// The items of `data` in the order of their bytes, from the commands in `found`. Data splits at `cuts`. Gives the item
// at each offset where one starts, and at the end. Without commands, the items are the whole bytecode as data, split at
// `cuts`. A target becomes the index of the item at the offset that `offset_of(target)` gives. A target without an
// item there keeps its value, and is outside.
template <typename OffsetOf>
Sequence Items(std::span<const uint8_t> data, const Found& found, const std::set<uint32_t>& cuts,
               std::map<uint32_t, uint32_t>& items, OffsetOf offset_of)
{
    Sequence sequence;
    const auto size = static_cast<uint32_t>(data.size());
    uint32_t at = 0;
    while (at < size)
    {
        items[at] = static_cast<uint32_t>(sequence.items.size());
        const auto read = found.reads.find(at);
        if (read != found.reads.end() && !read->second.raw)
        {
            sequence.items.push_back(read->second.command);
            at += read->second.size;
            continue;
        }

        // A run of data up to the next command, cut or end. A raw command stands alone.
        uint32_t end = read != found.reads.end() ? at + read->second.size : size;
        if (read == found.reads.end())
        {
            const auto next_read = found.reads.upper_bound(at);
            if (next_read != found.reads.end())
            {
                end = std::min(end, next_read->first);
            }

            const auto next_cut = cuts.upper_bound(at);
            if (next_cut != cuts.end())
            {
                end = std::min(end, *next_cut);
            }
        }

        Command item;
        item.kind = Kind::kData;
        item.bytes.assign(data.begin() + at, data.begin() + end);
        sequence.items.push_back(std::move(item));
        at = end;
    }
    items[size] = static_cast<uint32_t>(sequence.items.size());

    // The targets by item.
    for (Command& command : sequence.items)
    {
        if (!HasTarget(command.kind))
        {
            continue;
        }

        const std::optional<uint32_t> offset = offset_of(command.target);
        const auto item = offset ? items.find(*offset) : items.end();
        if (item == items.end())
        {
            command.target_outside = true;
            continue;
        }

        command.target = item->second;
    }

    return sequence;
}

} // namespace ntdlib::commands::detail
