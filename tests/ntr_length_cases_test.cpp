// SPDX-License-Identifier: MIT

// The length analysis's shared cases (testing/length_cases.h) on the DS's length model (ntdlib/ntr/length_model.h).
// Each case's sequence is the later consoles' bytecode, decoded into the common form of ntdlib::commands and encoded
// in the DS's bytecode. The timebase command becomes a wait of 0: the DS's timebase is always 48. A case that
// allocates tracks starts with the DS's allocation of them (0xFE). Each case is the one sequence of a sequence archive,
// at the case's start, with the case's volume. Program 0 plays a wave of 1 s that ends, and program 1 a wave of 0.1 s
// that loops, both with the fastest envelope. The timings are the cases', worked out by hand for each sequence.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/ntr/length_model.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sdat_files.h"

namespace
{

namespace cases = ntdlib::testing::length_cases;
namespace commands = ntdlib::commands;
namespace files = ntdlib::testing::sdat_files;
namespace ntr = ntdlib::ntr;
namespace sdat = ntdlib::sdat;
using files::Bytes;

// The cases that the DS can't play: it has no damper.
const std::set<std::string> kWithoutDamper = {"SEQ_DAMPER"};

// The lengths that the DS gives otherwise than the later consoles. In SEQ_REOPEN, the DS's open of a track keeps the
// track's wait. The Wii's open keeps it too. Track 1 still waits when track 0 opens it again at 48 and 96 ticks. It
// starts its part again when the wait runs out, at 120 and 144 ticks. The second start, at 1,500 ms, is the second
// pass. The fade starts two frames later.
struct Range
{
    long long min_ms = 0;
    long long max_ms = 0;
};

const std::map<std::string, Range> kDsLengths = {{"SEQ_REOPEN", {1495, 1515}}};

// The DS's bytecode of case `index`, and its start.
struct Ported
{
    Bytes bytes;
    uint32_t start = 0;
};

Ported Port(std::size_t index)
{
    const cases::Case& item = cases::Cases()[index];
    const Bytes later = cases::Bytecode(index, false);
    const std::vector<uint32_t> entries = {item.start};
    ntdlib::nwsnd::Decoded decoded = ntdlib::nwsnd::DecodeSequence(later, {}, ntdlib::nwsnd::Grammar{}, entries);

    // The DS's commands, with the console's usual bytes. The timebase becomes a wait of 0.
    commands::Sequence& sequence = decoded.sequence;
    for (commands::Command& command : sequence.items)
    {
        command.encoding = {};
        if (command.kind == commands::Kind::kTimebase)
        {
            command = {};
            command.kind = commands::Kind::kWait;
        }
    }

    // A sequence of several tracks allocates them first. The items after the allocation move up one.
    uint32_t entry = decoded.entries.at(0);
    if (item.tracks != 1)
    {
        for (commands::Command& command : sequence.items)
        {
            if (commands::HasTarget(command.kind) && !command.target_outside)
            {
                command.target++;
            }
        }
        commands::Command allocate;
        allocate.kind = commands::Kind::kAllocateTracks;
        allocate.value = {.value = commands::ToUnit(static_cast<int64_t>(item.tracks), commands::Scale{})};
        sequence.items.insert(sequence.items.begin() + entry, allocate);
    }

    const ntr::snd::Encoded encoded = ntr::snd::EncodeSequence(sequence);
    return {encoded.bytes, encoded.offsets.at(entry)};
}

// A wave of constant samples of 0x1000 at 32,768 Hz. A wave's length is its loop's start and length, in words of two
// samples, whether it loops or not.
files::Wave ConstantWave(uint32_t samples, bool loop)
{
    files::Wave wave{
        .encoding = 1, .loop = static_cast<uint8_t>(loop ? 1 : 0), .sample_rate = 32768, .loop_length = samples / 2};
    for (uint32_t i = 0; i < samples; i++)
    {
        wave.data.insert(wave.data.end(), {0x00, 0x10});
    }
    return wave;
}

// An archive whose sequence archive 0 holds `ported` at its start, at `volume`.
Bytes ArchiveFile(const Ported& ported, uint8_t volume)
{
    files::Archive spec;
    spec.items[1] = {files::SequenceArchiveItem(0, "CASE", {"CASE"})};
    spec.items[2] = {files::BankItem(1, {0, 0xFFFF, 0xFFFF, 0xFFFF}, "BANK")};
    spec.items[3] = {files::FileItem(2, "WAVE")};
    spec.items[4] = {files::PlayerItem(16, 0, 0, "PLAYER")};
    spec.files = {
        files::SequenceArchive({{.offset = ported.start, .volume = volume}}, ported.bytes),
        files::Bank({{.type = 1, .region = {.wave = 0}}, {.type = 1, .region = {.wave = 1}}}),
        files::WaveArchive({ConstantWave(32768, false), ConstantWave(3276, true)}),
    };
    return files::SoundArchive(spec);
}

std::optional<xsflib::rip::Timing> Measure(std::size_t index)
{
    const Bytes file = ArchiveFile(Port(index), cases::Cases()[index].volume);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    return ntr::MeasureSequence(archive, {0, 0});
}

// Every case but those in kWithoutDamper, by testing/length_cases.h's CheckCases, with the DS's lengths of kDsLengths.
// The DS's model waits for the other tracks' loops.
void TestCases()
{
    const std::vector<cases::Case>& all = cases::Cases();
    for (std::size_t i = 0; i < all.size(); i++)
    {
        const cases::Case& item = all[i];
        const std::string name = item.name;
        if (kWithoutDamper.contains(name))
        {
            continue;
        }

        const std::optional<xsflib::rip::Timing> timing = Measure(i);
        if (!xsflib::testing::Check(timing.has_value(), name + " has a length"))
        {
            continue;
        }

        long long min_ms = item.others_min_ms >= 0 ? item.others_min_ms : item.min_ms;
        long long max_ms = item.others_min_ms >= 0 ? item.others_max_ms : item.max_ms;
        if (const auto ds = kDsLengths.find(name); ds != kDsLengths.end())
        {
            min_ms = ds->second.min_ms;
            max_ms = ds->second.max_ms;
        }
        if (item.same_length_as >= 0)
        {
            const std::optional<xsflib::rip::Timing> other = Measure(static_cast<std::size_t>(item.same_length_as));
            min_ms = max_ms = other ? other->length_ms : -1;
        }
        xsflib::testing::Check(
            timing->length_ms >= min_ms && timing->length_ms <= max_ms && timing->fade_ms == item.fade_ms,
            name + "'s length is " + std::to_string(timing->length_ms) + " ms with a fade of " +
                std::to_string(timing->fade_ms) + " ms");
        xsflib::testing::Check(timing->silent == item.silent, name + (item.silent ? " is silent" : " isn't silent"));
    }
}

} // namespace

int main()
{
    TestCases();

    return xsflib::testing::Summary("DS length cases");
}
