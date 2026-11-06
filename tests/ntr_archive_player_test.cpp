// SPDX-License-Identifier: MIT

// Tests of the player of a sound archive's sequences on the model of the DS's sound driver
// (ntdlib/ntr/archive_player.h), on an archive that testing/sdat_files.h builds. The expected commands follow the SDK's
// sound archive player, and the expected addresses the loading in sdat/heap.h, worked out by hand.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/heap.h"
#include "ntdlib/testing/sdat_files.h"

namespace
{

namespace files = ntdlib::testing::sdat_files;
namespace ntr = ntdlib::ntr;
namespace sdat = ntdlib::sdat;
namespace snd = ntdlib::ntr::snd;
using files::Bytes;
using xsflib::testing::ErrorOf;

// Track 0 plays key 60 for 48 ticks, waits 48 more and ends.
const Bytes kNote = {0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF};

// The sequence SEQ_A (file 0) plays at volume 100 on the archive's player 1. That player may take channels 4 to 11. The
// sequence archive (file 1) holds one sequence at volume 127 on player 0. Both play bank 0 (file 2). The bank's one
// instrument plays the only wave of wave archive 0 (file 3).
files::Archive ArchiveSpec()
{
    files::Archive spec;
    spec.items[0] = {files::SequenceItem(0, 0, "SEQ_A", 1)};
    spec.items[1] = {files::SequenceArchiveItem(1, "SSAR", {"SE"})};
    spec.items[2] = {files::BankItem(2, {0, 0xFFFF, 0xFFFF, 0xFFFF}, "BANK")};
    spec.items[3] = {files::FileItem(3, "WAVE")};
    spec.items[4] = {files::PlayerItem(1, 0, 0, "PLAYER_0"), files::PlayerItem(1, 0x0FF0, 0, "PLAYER_1")};
    const files::Wave wave{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    spec.files = {files::Sequence(kNote), files::SequenceArchive({{.channel_priority = 50}}, kNote),
                  files::Bank({{.type = 1, .region = {.wave = 0}}}), files::WaveArchive({wave})};
    return spec;
}

Bytes ArchiveFile()
{
    return files::SoundArchive(ArchiveSpec());
}

// An archive whose sequence SEQ_A plays `sequence`, and whose bank releases its notes at `release`.
Bytes ArchiveFile(const Bytes& sequence, int release = 127)
{
    files::Archive spec = ArchiveSpec();
    spec.files[0] = files::Sequence(sequence);
    spec.files[2] = files::Bank({{.type = 1, .region = {.wave = 0, .release = static_cast<uint8_t>(release)}}});
    return files::SoundArchive(spec);
}

// Track 0 plays key 60, then key 62 on its channel with a tie, and ends. The bank's release of 100 keeps the channel
// for 305 frames after the end (see TestBusyWhileReleasing).
const Bytes kTiedNotes = {0xC8, 0x01, 0x3C, 0x7F, 0x30, 0x3E, 0x7F, 0x30, 0xFF};

uint32_t Align32(uint32_t value)
{
    return (value + 31) & ~31u;
}

uint32_t Word16(std::span<const uint8_t> memory, uint32_t address)
{
    return xsflib::Reader(memory).U16(address - ntr::ArchivePlayer::kMainMemory);
}

void TestStartCommands()
{
    const Bytes file = ArchiveFile();
    const sdat::Archive archive = sdat::Archive::Parse(file);
    const auto size = [&archive](uint32_t id)
    {
        return static_cast<uint32_t>(archive.File(id).size());
    };

    // A sequence's bank and wave archive go first, then its file. Player 1 gives the tracks channels 4 to 11. Volume
    // 100's level is -2.1 dB.
    Bytes memory(0x40000, 0);
    sdat::SoundHeap heap(memory, 0x02000000, 0x02010000, 0x02040000);
    const uint32_t bank = 0x02010000;
    const uint32_t sequence = Align32(Align32(bank + size(2)) + size(3));
    XSFLIB_CHECK(ntr::StartCommands(heap, archive, {0}) ==
                 (snd::CommandList{{snd::CommandId::kPrepareSequence, {0, sequence + 0x1C, 0, bank}},
                                   {snd::CommandId::kAllocatableChannels, {0, 0xFFFF, 0x0FF0, 0}},
                                   {snd::CommandId::kPlayerParameter, {0, 4, 64, 1}},
                                   {snd::CommandId::kPlayerParameter, {0, 6, static_cast<uint32_t>(-21), 2}},
                                   {snd::CommandId::kStartPreparedSequence, {0, 0, 0, 0}}}));

    // A sequence archive's file goes before the bank. Player 0 names no channels, and volume 127 requires no level. The
    // variables come after the channel priority.
    Bytes other(0x40000, 0);
    sdat::SoundHeap other_heap(other, 0x02000000, 0x02010000, 0x02040000);
    const uint32_t sequences = 0x02010000;
    const uint32_t other_bank = Align32(sequences + size(1));
    const uint32_t data = sequences + sdat::SequenceArchive::Parse(archive.File(1)).data_offset;
    ntr::Variables variables;
    variables.local[3] = 5;
    variables.global[1] = -2;
    XSFLIB_CHECK(ntr::StartCommands(other_heap, archive, {0, 0}, {.variables = variables}, 2) ==
                 (snd::CommandList{{snd::CommandId::kPrepareSequence, {2, data, 0, other_bank}},
                                   {snd::CommandId::kPlayerParameter, {2, 4, 50, 1}},
                                   {snd::CommandId::kPlayerLocalVariable, {2, 3, 5, 0}},
                                   {snd::CommandId::kGlobalVariable, {1, 0xFFFE, 0, 0}},
                                   {snd::CommandId::kStartPreparedSequence, {2, 0, 0, 0}}}));

    // A game's channels for the archive's players replace the archive's. 0 sends no channels, as the archive's 0
    // does.
    Bytes third(0x40000, 0);
    sdat::SoundHeap third_heap(third, 0x02000000, 0x02010000, 0x02040000);
    const snd::CommandList set = ntr::StartCommands(third_heap, archive, {0}, {.channels = {{1, 0x7FFF}}});
    XSFLIB_CHECK(set.size() == 5 &&
                 set[1] == (snd::Command{snd::CommandId::kAllocatableChannels, {0, 0xFFFF, 0x7FFF, 0}}));
    XSFLIB_CHECK(ntr::StartCommands(third_heap, archive, {0}, {.channels = {{1, 0}}}).size() == 4);
    const snd::CommandList named =
        ntr::StartCommands(third_heap, archive, {0, 0}, {.channels = {{0, 0x00F0}, {1, 0x7FFF}}});
    XSFLIB_CHECK(named.size() == 4 &&
                 named[1] == (snd::Command{snd::CommandId::kAllocatableChannels, {0, 0xFFFF, 0x00F0, 0}}));

    // The track channels follow the start, in their order.
    const ntr::GameSettings tracks = {.track_channels = {{0x000F, 0xA7FE}, {0x0030, 0x0003}}};
    const snd::CommandList after = ntr::StartCommands(third_heap, archive, {0, 0}, tracks, 3);
    XSFLIB_CHECK(after.size() == 5 &&
                 after[2] == (snd::Command{snd::CommandId::kStartPreparedSequence, {3, 0, 0, 0}}) &&
                 after[3] == (snd::Command{snd::CommandId::kAllocatableChannels, {3, 0x000F, 0xA7FE, 0}}) &&
                 after[4] == (snd::Command{snd::CommandId::kAllocatableChannels, {3, 0x0030, 0x0003, 0}}));

    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { ntr::StartCommands(other_heap, archive, {1}); }) ==
                 "the archive has no sequence 1");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { ntr::StartCommands(other_heap, archive, {0, 1}); }) ==
                 "sequence archive 0 has no sequence 1");

    // A volume above 127 takes 127's level, and the commands leave the player's volume alone.
    files::Archive loud = ArchiveSpec();
    loud.items[0][0].record[6] = 200;
    const Bytes loud_file = files::SoundArchive(loud);
    const sdat::Archive loud_archive = sdat::Archive::Parse(loud_file);
    Bytes loud_memory(0x40000, 0);
    sdat::SoundHeap loud_heap(loud_memory, 0x02000000, 0x02010000, 0x02040000);
    const snd::CommandList loud_list = ntr::StartCommands(loud_heap, loud_archive, {0});
    XSFLIB_CHECK(std::none_of(loud_list.begin(), loud_list.end(), [](const snd::Command& command)
                              { return command.id == snd::CommandId::kPlayerParameter && command.args[1] == 6; }));
}

void TestVariableCommands()
{
    ntr::Variables variables;
    variables.local[0] = 1;
    variables.local[15] = -1;
    variables.global[4] = 300;
    XSFLIB_CHECK(ntr::VariableCommands(variables, 3) ==
                 (snd::CommandList{{snd::CommandId::kPlayerLocalVariable, {3, 0, 1, 0}},
                                   {snd::CommandId::kPlayerLocalVariable, {3, 15, 0xFFFF, 0}},
                                   {snd::CommandId::kGlobalVariable, {4, 300, 0, 0}}}));
    XSFLIB_CHECK(ntr::VariableCommands({}).empty());

    // A host changes a variable while the sequence plays. The driver takes it at the next frame.
    const Bytes file = ArchiveFile();
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    player.RunFrame(out);
    ntr::Variables changed;
    changed.global[3] = 4;
    player.Send(ntr::VariableCommands(changed));
    XSFLIB_CHECK(Word16(player.MainMemory(), ntr::ArchivePlayer::kSharedWork + 0x266) == 0xFFFF);
    player.RunFrame(out);
    XSFLIB_CHECK(Word16(player.MainMemory(), ntr::ArchivePlayer::kSharedWork + 0x266) == 4);
}

// The commands of NNS_SndCaptureStartReverb: a timer of 0xFFB0FF / 16,000 = 1,047, 512 words in each half, channel 1
// at pan 0 and channel 3 at pan 127, both looped. The channels take format 1 for PCM16 and 0 for PCM8. The captures
// take bit 30 for PCM8, and bit 29 to loop.
void TestReverbCommands()
{
    const auto channel = [](uint32_t number, uint32_t address, uint32_t format, uint32_t pan)
    {
        return snd::Command{snd::CommandId::kSetUpPcmChannel,
                            {number | 1047 << 16, address, 512 | 30 << 24, format << 24 | 1 << 26 | pan << 16}};
    };
    const auto capture = [](uint32_t number, uint32_t address, uint32_t pcm8)
    {
        return snd::Command{snd::CommandId::kSetUpCapture, {address, 512, number << 31 | pcm8 << 30 | 1 << 29, 0}};
    };
    XSFLIB_CHECK(ntr::ReverbCommands({.size = 0x1000, .volume = 30}, 0x02003000) ==
                 (snd::CommandList{{snd::CommandId::kLockChannels, {0xA, 0, 0, 0}},
                                   channel(1, 0x02003000, 1, 0),
                                   capture(0, 0x02003000, 0),
                                   channel(3, 0x02003800, 1, 127),
                                   capture(1, 0x02003800, 0),
                                   {snd::CommandId::kStartTimer, {0xA, 3, 0, 0}}}));
    const snd::CommandList pcm8 = ntr::ReverbCommands({.size = 0x1000, .pcm8 = true, .volume = 30}, 0x02003000);
    XSFLIB_CHECK(pcm8.size() == 6 && pcm8[1] == channel(1, 0x02003000, 0, 0) && pcm8[2] == capture(0, 0x02003000, 1));
}

// A sequence with the reverb: the note's output, then its echo one half of the buffer later. The reverb's channels
// and captures share a timer of 1,047 periods of half the system clock: 2,094 system clocks. An echo comes 1,026
// periods after its capture (SoundUnit's captures), 2,098 output samples. Where a capture period falls against the
// output samples moves the first difference by up to 3 samples.
void TestReverb()
{
    const Bytes file = ArchiveFile();
    const sdat::Archive archive = sdat::Archive::Parse(file);
    const ntr::GameSettings settings = {.reverb = ntr::Reverb{.size = 0x1000, .volume = 64}};
    ntr::ArchivePlayer dry(archive);
    ntr::ArchivePlayer wet(archive);
    dry.Start({0, 0});
    wet.Start({0, 0}, settings);
    std::vector<ntr::SoundOutput> dry_out;
    std::vector<ntr::SoundOutput> wet_out;
    for (int frame = 0; frame < 100; frame++)
    {
        dry.RunFrame(dry_out);
        wet.RunFrame(wet_out);
    }
    XSFLIB_CHECK(wet.Unit().Busy(1) && wet.Unit().Busy(3) && wet.Unit().Read(0x04000508, 2) == 0x8080);
    const auto first = [](const std::vector<ntr::SoundOutput>& a, const std::vector<ntr::SoundOutput>& b)
    {
        const auto at = std::mismatch(a.begin(), a.end(), b.begin(), b.end());
        return static_cast<std::ptrdiff_t>(at.first - a.begin());
    };
    const std::vector<ntr::SoundOutput> silence(dry_out.size(), ntr::SoundOutput{0x200, 0x200});
    const std::ptrdiff_t sound = first(dry_out, silence);
    const std::ptrdiff_t echo = first(dry_out, wet_out);
    XSFLIB_CHECK(sound > 0 && echo - sound >= 2098 - 3 && echo - sound <= 2098 + 3);

    // NNS_SndCaptureStartReverb clears the buffer as it starts. The echoes of the first start don't play again.
    const auto buffer = [](const ntr::ArchivePlayer& player)
    {
        return player.MainMemory().subspan(ntr::ArchivePlayer::kReverbBuffer - ntr::ArchivePlayer::kMainMemory, 0x1000);
    };
    const auto zero = [](uint8_t byte)
    {
        return byte == 0;
    };
    XSFLIB_CHECK(!std::ranges::all_of(buffer(wet), zero));
    wet.Start({0, 0}, settings);
    XSFLIB_CHECK(std::ranges::all_of(buffer(wet), zero));

    // Channels 1 and 3 are the reverb's. A player that may take only those channels plays no note. A second start
    // with the reverb stops the first, and the reverb runs on.
    const ntr::GameSettings locked = {.channels = {{0, 0x000A}}, .reverb = settings.reverb};
    ntr::ArchivePlayer none(archive);
    none.Start({0, 0}, locked);
    std::vector<ntr::SoundOutput> none_out;
    for (int frame = 0; frame < 100; frame++)
    {
        none.RunFrame(none_out);
    }
    XSFLIB_CHECK(std::ranges::all_of(
        none_out, [](const ntr::SoundOutput& out) { return out == ntr::SoundOutput{0x200, 0x200}; }));
    none.Start({0, 0}, locked);
    none.RunFrame(none_out);
    XSFLIB_CHECK(none.Unit().Busy(1) && none.Unit().Busy(3) && none.Unit().Read(0x04000508, 2) == 0x8080);

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { none.Start({0, 0}, {.reverb = ntr::Reverb{.size = 0xE008}}); }) ==
                 "a reverb's buffer of 57352 bytes at 16000 Hz doesn't fit the player");
    // Each half of the buffer is a channel's and a capture's length in words.
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { none.Start({0, 0}, {.reverb = ntr::Reverb{.size = 0x1004}}); }) ==
                 "a reverb's buffer of 4100 bytes at 16000 Hz doesn't fit the player");
}

// The functions of the output modes, on parts worked out by hand from the steps of the games' functions (see
// ntr::OutputMode).
void TestOutputModes()
{
    using Samples = std::vector<int16_t>;
    ntr::OutputHistory history{};

    // kNone changes nothing.
    Samples left = {1, 2, 3};
    Samples right = {4, 5, 6};
    history[0] = 9;
    ntr::ApplyOutputMode(ntr::OutputMode::kNone, left, right, history);
    XSFLIB_CHECK(left == (Samples{1, 2, 3}) && right == (Samples{4, 5, 6}) && history[0] == 9);

    // kMono: (left + right + 1) >> 1 on both sides, with the shift rounding down.
    left = {1, -3, 0x7FFF, -0x8000};
    right = {2, -4, 0x7FFF, -0x8000};
    ntr::ApplyOutputMode(ntr::OutputMode::kMono, left, right, history);
    XSFLIB_CHECK(left == (Samples{2, -3, 0x7FFF, -0x8000}) && right == left && history[0] == 9);

    // kSurround. The differences are 90, 180, 270 and 360. Samples 3 and 2 add 180 and 90 on the left and take them
    // off on the right. Samples 0 and 1 add the history's 5 and -7. The history then holds 270 and 360.
    history = {};
    history[0] = 5;
    history[1] = -7;
    left = {100, 200, 300, 400};
    right = {10, 20, 30, 40};
    ntr::ApplyOutputMode(ntr::OutputMode::kSurround, left, right, history);
    XSFLIB_CHECK(left == (Samples{105, 193, 390, 580}) && right == (Samples{5, 27, -60, -140}));
    XSFLIB_CHECK(history[0] == 270 && history[1] == 360 && history[2] == 0);

    // The differences don't stop at 16 bits: 0x7000 - -0x7000 is 0xE000. Sample 4 then takes 0x2000 + 0xE000 and stops
    // at 0x7FFF on the left and -0x8000 on the right. The history's differences stop at 16 bits: 0x2000 and 0x7FFF.
    history = {};
    left = {0, 0, 0x7000, 0, 0x2000, 0x7000};
    right = {0, 0, -0x7000, 0, 0, -0x7000};
    ntr::ApplyOutputMode(ntr::OutputMode::kSurround, left, right, history);
    XSFLIB_CHECK(left == (Samples{0, 0, 0x7000, 0, 0x7FFF, 0x7000}));
    XSFLIB_CHECK(right == (Samples{0, 0, -0x7000, 0, -0x8000, -0x7000}));
    XSFLIB_CHECK(history[0] == 0x2000 && history[1] == 0x7FFF);

    // The next part's first two samples add them: 1 + 0x7FFF stops at 0x7FFF.
    left = {1, 1, 1, 1};
    right = {0, 0, 0, 0};
    ntr::ApplyOutputMode(ntr::OutputMode::kSurround, left, right, history);
    XSFLIB_CHECK(left == (Samples{0x2001, 0x7FFF, 2, 2}) && right == (Samples{-0x2000, -0x7FFF, -1, -1}));
    XSFLIB_CHECK(history[0] == 1 && history[1] == 1);

    // kCrossfeed on 26 samples: one run of 24, then 2. In the run, place k adds pair k of the history, and pair k takes
    // the quarters of the sums: (right + 1) >> 2 first, then (left + 1) >> 2.
    //   Place 0: 400 + 8 = 408 and 0 - 8 = -8. Pair 0 takes (-7 >> 2) = -2 and (409 >> 2) = 102.
    //   Place 1: 0 + 4 = 4 and -200 + 12 = -188. Pair 1 takes (-187 >> 2) = -47 and (5 >> 2) = 1.
    //   Place 2: 0x2000 + 0x7000 = 0x9000 stops at 0x7FFF. Pair 2 takes 0 and (0x9001 >> 2) = 0x2400.
    //   Place 23: 0 + 40 and 0 + 80. Pair 23 takes (81 >> 2) = 20 and (41 >> 2) = 10.
    // The last 2 samples add pairs 0 and 1: 0 - 2 = -2, 0x7FC0 + 102 stops at 0x7FFF, 100 - 47 = 53 and
    // 0 + 1 = 1. Pairs 2 to 23 move to 0 to 21. Pairs 22 and 23 take the quarters of the stopped samples:
    // (0x8000 >> 2) = 0x2000 and (-1 >> 2) = -1, then (2 >> 2) = 0 and (54 >> 2) = 13.
    history = {};
    history[0] = 8;
    history[1] = -8;
    history[2] = 4;
    history[3] = 12;
    history[4] = 0x7000;
    history[46] = 40;
    history[47] = 80;
    left.assign(26, 0);
    right.assign(26, 0);
    left[0] = 400;
    left[2] = 0x2000;
    left[25] = 100;
    right[1] = -200;
    right[24] = 0x7FC0;
    ntr::ApplyOutputMode(ntr::OutputMode::kCrossfeed, left, right, history);
    Samples expected_left(26, 0);
    Samples expected_right(26, 0);
    expected_left[0] = 408;
    expected_left[1] = 4;
    expected_left[2] = 0x7FFF;
    expected_left[23] = 40;
    expected_left[24] = -2;
    expected_left[25] = 53;
    expected_right[0] = -8;
    expected_right[1] = -188;
    expected_right[23] = 80;
    expected_right[24] = 0x7FFF;
    expected_right[25] = 1;
    XSFLIB_CHECK(left == expected_left && right == expected_right);
    ntr::OutputHistory expected_history{};
    expected_history[1] = 0x2400;
    expected_history[42] = 20;
    expected_history[43] = 10;
    expected_history[44] = 0x2000;
    expected_history[45] = -1;
    expected_history[46] = 0;
    expected_history[47] = 13;
    XSFLIB_CHECK(history == expected_history);
}

// The commands of NNS_SndCaptureStartEffect that a game's ARM9 code sent for an effect of 0x1000 bytes in PCM16
// at 32,000 Hz, in 2 parts, with its buffer at 0x021DA420 (Mario Kart DS, USA): a timer of 512, 512 words in each half,
// channel 1 at pan 0 and channel 3 at pan 127, both looped at volume 127. Alarm 0 fires first after 0x2100 ticks, then
// every 0x2000. The output takes channel 1 on the left and channel 3 on the right, and neither goes to the mixer. The
// start takes alarm 0 as well.
void TestOutputEffectCommands()
{
    const ntr::OutputEffect effect;
    XSFLIB_CHECK(ntr::OutputEffectCommands(effect, 0x021DA420) ==
                 (snd::CommandList{{snd::CommandId::kLockChannels, {0xA, 0, 0, 0}},
                                   {snd::CommandId::kSetUpPcmChannel, {0x02000001, 0x021DA420, 0x7F000200, 0x05000000}},
                                   {snd::CommandId::kSetUpCapture, {0x021DA420, 0x200, 0x20000000, 0}},
                                   {snd::CommandId::kSetUpPcmChannel, {0x02000003, 0x021DAC20, 0x7F000200, 0x057F0000}},
                                   {snd::CommandId::kSetUpCapture, {0x021DAC20, 0x200, 0xA0000000, 0}},
                                   {snd::CommandId::kSetUpAlarm, {0, 0x2100, 0x2000, 1}},
                                   {snd::CommandId::kOutputSelector, {1, 2, 1, 1}},
                                   {snd::CommandId::kStartTimer, {0xA, 3, 1, 0}}}));

    // PCM8: the channels take format 0, and the captures bit 30. A half holds 0x800 samples, a part 0x400. A sample
    // takes 512 / 32 = 16 ticks: a period of 0x4000 ticks, and a first alarm 32 samples later, at 0x4200.
    const snd::CommandList pcm8 = ntr::OutputEffectCommands({.pcm8 = true}, 0x02003000, 2);
    XSFLIB_CHECK(pcm8.size() == 8);
    XSFLIB_CHECK(pcm8[1] ==
                 (snd::Command{snd::CommandId::kSetUpPcmChannel, {0x02000001, 0x02003000, 0x7F000200, 0x04000000}}));
    XSFLIB_CHECK(pcm8[4] == (snd::Command{snd::CommandId::kSetUpCapture, {0x02003800, 0x200, 0xE0000000, 0}}));
    XSFLIB_CHECK(pcm8[5] == (snd::Command{snd::CommandId::kSetUpAlarm, {0, 0x4200, 0x4000, 2}}));

    // At 16,000 Hz: 0xFFB0FF / 16,000 = 1,047, and (1,047 + 16) & ~31 = 1,056. A sample takes 33 ticks. A part of 512
    // samples takes 16,896 ticks, and the first alarm comes 16 samples later, at 17,424.
    const snd::CommandList slow = ntr::OutputEffectCommands({.sample_rate = 16000}, 0x02003000);
    XSFLIB_CHECK(slow[1].args[0] == (0x420u << 16 | 1) && slow[3].args[0] == (0x420u << 16 | 3));
    XSFLIB_CHECK(slow[5] == (snd::Command{snd::CommandId::kSetUpAlarm, {0, 17424, 16896, 1}}));

    // The games' setter: a surround decay of 0x3000 as the surround starts, and of 0 as it ends.
    XSFLIB_CHECK(ntr::OutputModeCommands(ntr::OutputMode::kNone, ntr::OutputMode::kSurround) ==
                 (snd::CommandList{{snd::CommandId::kSurroundDecay, {0x3000, 0, 0, 0}}}));
    XSFLIB_CHECK(ntr::OutputModeCommands(ntr::OutputMode::kSurround, ntr::OutputMode::kMono) ==
                 (snd::CommandList{{snd::CommandId::kSurroundDecay, {0, 0, 0, 0}}}));
    XSFLIB_CHECK(ntr::OutputModeCommands(ntr::OutputMode::kMono, ntr::OutputMode::kCrossfeed).empty());
    XSFLIB_CHECK(ntr::OutputModeCommands(ntr::OutputMode::kSurround, ntr::OutputMode::kSurround).empty());

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [] { ntr::OutputEffectCommands({.mode = static_cast<ntr::OutputMode>(4)}, 0x02003000); }) ==
                 "no output mode 4");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([] { ntr::OutputEffectCommands({.interval = 3}, 0x02003000); }) ==
                 "the SDK can't lay out an output effect of 4096 bytes in 3 parts at 32000 Hz");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([] { ntr::OutputEffectCommands({.sample_rate = 200}, 0x02003000); }) ==
                 "the SDK can't lay out an output effect of 4096 bytes in 2 parts at 200 Hz");
}

// Track 0 pans hard left, plays key 60 for 48 ticks and ends.
const Bytes kLeftNote = {0xC0, 0x00, 0x3C, 0x7F, 0x30, 0xFF};

std::span<const uint8_t> EffectBuffer(const ntr::ArchivePlayer& player)
{
    return player.MainMemory().subspan(ntr::ArchivePlayer::kEffectBuffer - ntr::ArchivePlayer::kMainMemory, 0x1000);
}

int16_t Sample16(std::span<const uint8_t> buffer, std::size_t index)
{
    return static_cast<int16_t>(buffer[2 * index] | buffer[2 * index + 1] << 8);
}

ntr::GameSettings EffectSettings(ntr::OutputMode mode)
{
    return {.effect = ntr::OutputEffect{.mode = mode}};
}

// SEQ_A with the output effect of TestOutputEffectCommands. Its archive player gives it channels 4 to 11.
void TestOutputEffect()
{
    const Bytes file = ArchiveFile(kLeftNote);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    const auto silent = [](const ntr::SoundOutput& out)
    {
        return out == ntr::SoundOutput{0x200, 0x200};
    };
    const auto first_sound = [&silent](const std::vector<ntr::SoundOutput>& out)
    {
        return std::ranges::find_if_not(out, silent) - out.begin();
    };

    // The effect's channels and captures share a timer of 512 periods of half the system clock: one output sample.
    // Without a mode, the output plays the mixer's sound one half of the buffer later: 1,024 samples, and the 2 of
    // SoundUnit's captures (see TestReverb). The output takes channels 1 and 3 in place of the mixer. The SOUNDCNT
    // register holds the master volume of 127, the selector's 1 and 2 at bits 8 and 10, and bits 12, 13 and 15.
    ntr::ArchivePlayer dry(archive);
    ntr::ArchivePlayer wet(archive);
    dry.Start({0});
    wet.Start({0}, EffectSettings(ntr::OutputMode::kNone));
    std::vector<ntr::SoundOutput> dry_out;
    std::vector<ntr::SoundOutput> wet_out;
    int dry_frames = 0;
    int wet_frames = 0;
    for (int frame = 0; frame < 300; frame++)
    {
        dry.RunFrame(dry_out);
        wet.RunFrame(wet_out);
        dry_frames += dry.Busy() ? 1 : 0;
        wet_frames += wet.Busy() ? 1 : 0;
    }
    XSFLIB_CHECK(wet.Unit().Read(0x04000500, 2) == 0xB97F && wet.Unit().Read(0x04000508, 2) == 0x8080);
    const std::ptrdiff_t sound = first_sound(dry_out);
    const std::ptrdiff_t echo = first_sound(wet_out);
    XSFLIB_CHECK(sound > 0 && echo - sound >= 1026 - 3 && echo - sound <= 1026 + 3);

    // The player stays busy until the capture has written zeros over the last sound, a pass of 1,024 samples: about 6
    // frames of 170.5 samples. One frame more lets the channels play the samples that they read ahead.
    XSFLIB_CHECK(dry_frames > 90 && wet_frames >= dry_frames + 5 && wet_frames <= dry_frames + 9);
    XSFLIB_CHECK(std::ranges::all_of(EffectBuffer(wet), [](uint8_t byte) { return byte == 0; }));

    // The note plays on the left alone. kMono gives both sides the same samples. The first RunFrame ends with the frame
    // that starts the effect. The first alarm comes 0x2100 ticks of 64 system clocks later: 540,672 clocks, 3.1 frames
    // of 174,592. The fifth RunFrame therefore holds it. Before it, the right half's first part holds zeros. After it,
    // the left half's first part holds (left + 1) >> 1, and the right half the same. The capture writes the part's last
    // samples in the frame of the alarm.
    ntr::ArchivePlayer mono(archive);
    mono.Start({0}, EffectSettings(ntr::OutputMode::kMono));
    std::vector<ntr::SoundOutput> mono_out;
    for (int frame = 0; frame < 4; frame++)
    {
        mono.RunFrame(mono_out);
    }
    const std::vector<uint8_t> before(EffectBuffer(mono).begin(), EffectBuffer(mono).end());
    XSFLIB_CHECK(std::all_of(before.begin() + 0x800, before.begin() + 0xC00, [](uint8_t byte) { return byte == 0; }));
    XSFLIB_CHECK(!std::all_of(before.begin(), before.begin() + 0x400, [](uint8_t byte) { return byte == 0; }));
    mono.RunFrame(mono_out);
    const std::span<const uint8_t> after = EffectBuffer(mono);
    bool halved = true;
    for (std::size_t i = 0; i < 500; i++)
    {
        halved = halved && Sample16(after, i) == static_cast<int16_t>((Sample16(before, i) + 1) >> 1);
    }
    XSFLIB_CHECK(halved && std::equal(after.begin(), after.begin() + 0x400, after.begin() + 0x800));
    for (int frame = 0; frame < 100; frame++)
    {
        mono.RunFrame(mono_out);
    }
    XSFLIB_CHECK(!std::ranges::all_of(mono_out, silent));
    XSFLIB_CHECK(std::ranges::all_of(mono_out, [](const ntr::SoundOutput& out) { return out.left == out.right; }));

    // kSurround puts the left side's differences on the right with a minus. The dry note gives the right nothing.
    ntr::ArchivePlayer surround(archive);
    surround.Start({0}, EffectSettings(ntr::OutputMode::kSurround));
    std::vector<ntr::SoundOutput> surround_out;
    for (int frame = 0; frame < 100; frame++)
    {
        surround.RunFrame(surround_out);
    }
    XSFLIB_CHECK(std::ranges::all_of(dry_out, [](const ntr::SoundOutput& out) { return out.right == 0x200; }));
    XSFLIB_CHECK(!std::ranges::all_of(surround_out, [](const ntr::SoundOutput& out) { return out.right == 0x200; }));

    // A snapshot holds the effect: its alarms, its part, its mode and its history.
    const ntr::ArchivePlayer::Snapshot snapshot = surround.Save(nullptr);
    std::vector<ntr::SoundOutput> played;
    for (int frame = 0; frame < 60; frame++)
    {
        surround.RunFrame(played);
    }
    surround.Restore(snapshot);
    std::vector<ntr::SoundOutput> again;
    for (int frame = 0; frame < 60; frame++)
    {
        surround.RunFrame(again);
    }
    XSFLIB_CHECK(again == played);

    // A restore to a snapshot from before the effect leaves the effect out. The player then plays as one that never
    // ran it.
    ntr::ArchivePlayer restored(archive);
    const ntr::ArchivePlayer::Snapshot unstarted = restored.Save(nullptr);
    restored.Start({0}, EffectSettings(ntr::OutputMode::kSurround));
    std::vector<ntr::SoundOutput> ignored;
    for (int frame = 0; frame < 30; frame++)
    {
        restored.RunFrame(ignored);
    }
    restored.Restore(unstarted);
    restored.Start({0});
    std::vector<ntr::SoundOutput> restored_out;
    for (int frame = 0; frame < 300; frame++)
    {
        restored.RunFrame(restored_out);
    }
    XSFLIB_CHECK(restored_out == dry_out);

    // A second start clears the buffer. NNS_SndCaptureStartEffect does the same.
    ntr::ArchivePlayer again_started(archive);
    again_started.Start({0}, EffectSettings(ntr::OutputMode::kSurround));
    for (int frame = 0; frame < 10; frame++)
    {
        again_started.RunFrame(ignored);
    }
    XSFLIB_CHECK(!std::ranges::all_of(EffectBuffer(again_started), [](uint8_t byte) { return byte == 0; }));
    again_started.Start({0}, EffectSettings(ntr::OutputMode::kMono));
    XSFLIB_CHECK(std::ranges::all_of(EffectBuffer(again_started), [](uint8_t byte) { return byte == 0; }));

    // The SDK runs the reverb or the output effect, not both.
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { dry.Start({0}, {.reverb = ntr::Reverb{}, .effect = ntr::OutputEffect{}}); }) ==
                 "the SDK runs a reverb or an output effect, not both");
    ntr::ArchivePlayer reverb(archive);
    reverb.Start({0}, {.reverb = ntr::Reverb{.volume = 30}});
    reverb.RunFrame(ignored);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { reverb.Start({0}, EffectSettings(ntr::OutputMode::kSurround)); }) ==
        "a reverb runs: the SDK starts no output effect");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { surround.Start({0}, {.reverb = ntr::Reverb{}}); }) ==
                 "an output effect runs: the SDK starts no reverb");
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { dry.Start({0}, {.effect = ntr::OutputEffect{.size = 0x10000}}); }) ==
        "an output effect's buffer of 65536 bytes doesn't fit the player");

    // The enhanced mode checks the effect and leaves it out: the captures stay off, and the mixer reaches the output.
    ntr::ArchivePlayerOptions options;
    options.enhanced = true;
    ntr::ArchivePlayer enhanced(archive, options);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { enhanced.Start({0}, {.effect = ntr::OutputEffect{.interval = 3}}); }) ==
        "the SDK can't lay out an output effect of 4096 bytes in 3 parts at 32000 Hz");
    enhanced.Start({0}, EffectSettings(ntr::OutputMode::kSurround));
    std::vector<float> enhanced_out;
    for (int frame = 0; frame < 20; frame++)
    {
        enhanced.RunFrame(enhanced_out);
    }
    XSFLIB_CHECK(enhanced.Unit().Read(0x04000508, 2) == 0 && (enhanced.Unit().Read(0x04000500, 2) & 0x3F00) == 0);
    XSFLIB_CHECK(std::ranges::any_of(enhanced_out, [](float sample) { return sample != 0.0f; }));
}

// The note events of a sequence with two tracks. The offsets count from the start of the sequence's data. So do the
// decoder's offsets. Track 2 is the driver's second track: the events give the track's number in the player. At tick
// 48, track 0's note comes before track 2's: the driver steps a player's tracks in order.
void TestNotes()
{
    const Bytes sequence = {
        0xFE, 0x05, 0x00,             // 0: tracks 0 and 2
        0x93, 0x02, 0x15, 0x00, 0x00, // 3: track 2 starts at 0x15
        0xC3, 0x02,                   // 8: a transpose of 2
        0x3C, 0x7F, 0x30,             // 10: key 60 (62), 48 ticks
        0xC7, 0x00,                   // 13: no note wait
        0x3E, 0x64, 0x00,             // 15: key 62 (64) at velocity 100, until the track releases it
        0x80, 0x30,                   // 18: a wait of 48 ticks
        0xFF,                         // 20
        0xC8, 0x01,                   // 21: tie
        0x40, 0x50, 0x18,             // 23: key 64 at velocity 80, 24 ticks
        0x80, 0x18,                   // 26: a wait of 24 ticks
        0x43, 0x60, 0x18,             // 28: key 67 at velocity 96, on the channel of key 64
        0x80, 0x30,                   // 31: a wait of 48 ticks
        0xFF,                         // 33
    };
    const Bytes file = ArchiveFile(sequence);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    std::vector<snd::NoteEvent> notes;
    player.on_note = [&notes](const snd::NoteEvent& note)
    {
        notes.push_back(note);
    };
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    for (int frame = 0; frame < 200; frame++)
    {
        player.RunFrame(out);
    }

    XSFLIB_CHECK(notes.size() == 4);
    if (notes.size() != 4)
    {
        return;
    }
    // Player 1 of the archive gives the tracks channels 4 to 11.
    const int first = notes[0].channel;
    const int second = notes[1].channel;
    XSFLIB_CHECK(first >= 4 && first <= 11 && second >= 4 && second <= 11 && first != second);
    XSFLIB_CHECK(notes[0] == (snd::NoteEvent{.player = 0,
                                             .track = 0,
                                             .offset = 10,
                                             .key = 62,
                                             .velocity = 127,
                                             .length = 48,
                                             .channel = first,
                                             .tie = false}));
    XSFLIB_CHECK(notes[1] == (snd::NoteEvent{.player = 0,
                                             .track = 2,
                                             .offset = 23,
                                             .key = 64,
                                             .velocity = 80,
                                             .length = 24,
                                             .channel = second,
                                             .tie = false}));
    XSFLIB_CHECK(notes[3] == (snd::NoteEvent{.player = 0,
                                             .track = 2,
                                             .offset = 28,
                                             .key = 67,
                                             .velocity = 96,
                                             .length = 24,
                                             .channel = second,
                                             .tie = true}));
    XSFLIB_CHECK(notes[2].channel >= 4 && notes[2].channel <= 11);
    XSFLIB_CHECK(notes[2] == (snd::NoteEvent{.player = 0,
                                             .track = 0,
                                             .offset = 15,
                                             .key = 64,
                                             .velocity = 100,
                                             .length = -1,
                                             .channel = notes[2].channel,
                                             .tie = false}));
    for (const snd::NoteEvent& note : notes)
    {
        const std::optional<snd::DecodedCommand> command = snd::DecodeCommand(sequence, note.offset);
        XSFLIB_CHECK(command && command->command.kind == ntdlib::commands::Kind::kNote);
    }
}

// Plays `file`'s SEQ_A to its end, at most 1,000 frames, after `set_up` has set the player up.
template <typename SetUp>
std::vector<ntr::SoundOutput> PlayToEnd(const Bytes& file, const SetUp& set_up)
{
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    set_up(player);
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    int frames = 0;
    do
    {
        player.RunFrame(out);
        frames++;
    } while (player.Busy() && frames < 1000);
    return out;
}

bool Silent(std::span<const ntr::SoundOutput> samples)
{
    return std::ranges::all_of(samples,
                               [](const ntr::SoundOutput& sample) { return sample == ntr::SoundOutput{0x200, 0x200}; });
}

void TestMutes()
{
    // A muted track plays as before, release included, and nothing of it reaches the output. The tie keeps the
    // channel. The release comes after the track has ended and let the channel go.
    const Bytes file = ArchiveFile(kTiedNotes, 100);
    const std::vector<ntr::SoundOutput> played = PlayToEnd(file, [](ntr::ArchivePlayer&) {});
    const std::vector<ntr::SoundOutput> muted =
        PlayToEnd(file, [](ntr::ArchivePlayer& player) { player.MuteTrack(0, 0, true); });
    XSFLIB_CHECK(!Silent(played));
    XSFLIB_CHECK(muted.size() == played.size() && Silent(muted));

    // The mutes of other tracks and players change nothing.
    XSFLIB_CHECK(PlayToEnd(file,
                           [](ntr::ArchivePlayer& player)
                           {
                               player.MuteTrack(0, 1, true);
                               player.MuteTrack(1, 0, true);
                           }) == played);

    // A mute names the driver's player too.
    const sdat::Archive two_archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer two(two_archive);
    two.Start({0});
    two.Start({0}, {}, 1);
    const auto frame_of = [&two]
    {
        std::vector<ntr::SoundOutput> samples;
        two.RunFrame(samples);
        return samples;
    };
    for (int frame = 0; frame < 4; frame++)
    {
        two.RunFrame();
    }
    two.MuteTrack(0, 0, true);
    XSFLIB_CHECK(!Silent(frame_of()));
    two.MuteTrack(1, 0, true);
    XSFLIB_CHECK(Silent(frame_of()));
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { two.Start({0}, {}, 16); }) == "no player 16");

    // A change takes effect at the next frame.
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    int channel = -1;
    player.on_note = [&channel](const snd::NoteEvent& note)
    {
        channel = note.channel;
    };
    player.MuteTrack(0, 0, true);
    XSFLIB_CHECK(player.IsTrackMuted(0, 0) && !player.IsTrackMuted(0, 1) && !player.IsTrackMuted(1, 0));
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    for (int frame = 0; frame < 20; frame++)
    {
        player.RunFrame(out);
    }
    const std::size_t unmuted = out.size();
    player.MuteTrack(0, 0, false);
    XSFLIB_CHECK(!player.IsTrackMuted(0, 0));
    for (int frame = 0; frame < 20; frame++)
    {
        player.RunFrame(out);
    }
    XSFLIB_CHECK(Silent(std::span(out).first(unmuted)));
    XSFLIB_CHECK(std::equal(out.begin() + static_cast<std::ptrdiff_t>(unmuted), out.end(),
                            played.begin() + static_cast<std::ptrdiff_t>(unmuted)));

    // A channel that the ARM9 locks and sets up plays no track's note. The mute of the track whose note it played
    // no longer reaches it. The mutes stay through a restore, and the channels go back to their tracks then.
    XSFLIB_CHECK(channel >= 4 && channel <= 11);
    if (channel < 0)
    {
        return;
    }
    player.MuteTrack(0, 0, true);
    const ntr::ArchivePlayer::Snapshot snapshot = player.Save(nullptr);
    const auto bit = 1u << channel;
    player.Send({{snd::CommandId::kLockChannels, {bit, 0, 0, 0}},
                 {snd::CommandId::kSetUpPcmChannel,
                  {static_cast<uint32_t>(channel) | 0x400u << 16, ntr::ArchivePlayer::kFiles, 127u << 24 | 0x100,
                   1u << 26 | 64u << 16}},
                 {snd::CommandId::kStartTimer, {bit, 0, 0, 0}}});
    player.RunFrame(out);
    std::vector<ntr::SoundOutput> set_up;
    player.RunFrame(set_up);
    XSFLIB_CHECK(!Silent(set_up));
    player.MuteTrack(0, 0, false);
    player.Restore(snapshot);
    XSFLIB_CHECK(!player.IsTrackMuted(0, 0));
    player.MuteTrack(0, 0, true);
    std::vector<ntr::SoundOutput> restored;
    player.RunFrame(restored);
    XSFLIB_CHECK(Silent(restored));
    player.MuteTrack(0, 0, false);
    restored.clear();
    player.RunFrame(restored);
    XSFLIB_CHECK(!Silent(restored));

    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { player.MuteTrack(0, 16, true); }) == "no track 16");
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { player.IsTrackMuted(0, -1); }) == "no track -1");
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { player.MuteTrack(16, 0, true); }) == "no player 16");
}

constexpr std::array<int32_t, 2> kNoPeaks = {};

// The largest magnitude of a side of `samples` on the scale of 16-bit samples.
std::array<int32_t, 2> OutputPeaks(std::span<const ntr::SoundOutput> samples)
{
    std::array<int32_t, 2> peaks{};
    for (const ntr::SoundOutput& sample : samples)
    {
        peaks[0] = std::max(peaks[0], std::abs((sample.left - 0x200) * 64));
        peaks[1] = std::max(peaks[1], std::abs((sample.right - 0x200) * 64));
    }
    return peaks;
}

void TestMeters()
{
    // A track's meter follows the output of its channels, before the cut to 10 bits. With one track, each frame's
    // peaks lie within 64 of the output's. The meters change nothing in the output.
    const Bytes file = ArchiveFile(kTiedNotes, 100);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    player.MeterTracks(0);
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    bool sounded = false;
    bool near = true;
    bool others = true;
    for (int frame = 0; frame < 400; frame++)
    {
        const std::size_t start = out.size();
        player.RunFrame(out);
        const std::array<int32_t, 2> peaks = player.TrackPeaks(0);
        const std::array<int32_t, 2> expected = OutputPeaks(std::span(out).subspan(start));
        sounded = sounded || peaks[0] > 0;
        near = near && std::abs(peaks[0] - expected[0]) < 64 && std::abs(peaks[1] - expected[1]) < 64;
        others = others && player.TrackPeaks(1) == kNoPeaks && player.TrackPeaks(15) == kNoPeaks;
    }
    XSFLIB_CHECK(sounded && near && others);
    const std::vector<ntr::SoundOutput> played = PlayToEnd(file, [](ntr::ArchivePlayer&) {});
    XSFLIB_CHECK(std::equal(out.begin(), out.end(), played.begin()));

    // The peaks stay through a restore. A muted track's meter, another player's meters and no meters give 0, and so
    // does a frame without mixing.
    const std::array<int32_t, 2> peaks = player.TrackPeaks(0);
    const ntr::ArchivePlayer::Snapshot snapshot = player.Save(nullptr);
    player.Restore(snapshot);
    XSFLIB_CHECK(player.TrackPeaks(0) == peaks);

    ntr::ArchivePlayer other(archive);
    other.Start({0});
    const auto peaks_after = [&other](int frames)
    {
        std::vector<ntr::SoundOutput> samples;
        for (int frame = 0; frame < frames; frame++)
        {
            other.RunFrame(samples);
        }
        return other.TrackPeaks(0);
    };
    other.MeterTracks(0);
    XSFLIB_CHECK(peaks_after(20) != kNoPeaks);
    other.MuteTrack(0, 0, true);
    XSFLIB_CHECK(peaks_after(1) == kNoPeaks);
    other.MuteTrack(0, 0, false);
    XSFLIB_CHECK(peaks_after(1) != kNoPeaks);
    other.RunFrame();
    XSFLIB_CHECK(other.TrackPeaks(0) == kNoPeaks);
    XSFLIB_CHECK(peaks_after(1) != kNoPeaks);
    other.MeterTracks(1);
    XSFLIB_CHECK(peaks_after(1) == kNoPeaks);
    other.MeterTracks(-1);
    XSFLIB_CHECK(peaks_after(1) == kNoPeaks);

    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { other.MeterTracks(16); }) == "no player 16");
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { other.TrackPeaks(16); }) == "no track 16");
}

// Frames without mixing leave the player where mixed frames do.
void TestFramesWithoutMixing()
{
    const Bytes file = ArchiveFile(kTiedNotes, 100);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer mixed(archive);
    ntr::ArchivePlayer skipped(archive);
    mixed.Start({0});
    skipped.Start({0});
    std::vector<ntr::SoundOutput> lead;
    for (int frame = 0; frame < 150; frame++)
    {
        mixed.RunFrame(lead);
        skipped.RunFrame();
    }
    XSFLIB_CHECK(mixed.Clock() == skipped.Clock());
    XSFLIB_CHECK(std::ranges::equal(mixed.MainMemory(), skipped.MainMemory()));
    std::vector<ntr::SoundOutput> after_mixed;
    std::vector<ntr::SoundOutput> after_skipped;
    for (int frame = 0; frame < 100; frame++)
    {
        mixed.RunFrame(after_mixed);
        skipped.RunFrame(after_skipped);
    }
    XSFLIB_CHECK(!Silent(after_mixed) && after_mixed == after_skipped);
}

// Track 1 waits 0 ticks and jumps back, without end. In its first tick, after kCommandBudget commands, the frame throws
// at the wait, at offset 0x0B.
void TestHang()
{
    const Bytes sequence = {
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x0B, 0x00, 0x00, // 3: track 1 starts at 0x0B
        0x80, 0x30,                   // 8: a wait of 48 ticks
        0xFF,                         // 10
        0x80, 0x00,                   // 11: a wait of 0 ticks
        0x94, 0x0B, 0x00, 0x00,       // 13: back to 11
    };
    const Bytes file = ArchiveFile(sequence);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    player.Start({0}, {}, 2);

    std::optional<snd::Hang> hang;
    std::vector<ntr::SoundOutput> out;
    try
    {
        for (int frame = 0; frame < 4; frame++)
        {
            player.RunFrame(out);
        }
    }
    catch (const snd::Hang& e)
    {
        hang = e;
    }

    XSFLIB_CHECK(hang && hang->PlayerIndex() == 2 && hang->TrackIndex() == 1 && hang->Offset() == 0x0B);
    XSFLIB_CHECK(hang &&
                 std::string(hang->what()) ==
                     "track 1 of player 2 runs 100000 commands in a tick without waiting, up to offset 0xB: the "
                     "sequence hangs");
}

void TestPlay()
{
    const Bytes file = ArchiveFile();
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);

    // The shared work that the ARM9's SND_Init sets up.
    XSFLIB_CHECK(Word16(player.MainMemory(), ntr::ArchivePlayer::kSharedWork + 0x20) == 0xFFFF);
    XSFLIB_CHECK(Word16(player.MainMemory(), ntr::ArchivePlayer::kSharedWork + 0x260) == 0xFFFF);
    XSFLIB_CHECK(!player.Busy());

    // At 120 beats a minute and 48 ticks a beat, the driver steps half a tick a frame. The note and the wait take 192
    // frames, about 1 s.
    ntr::Variables variables;
    variables.global[2] = 7;
    player.Start({0}, {.variables = variables});
    XSFLIB_CHECK(player.Busy());
    uint32_t writes = 0;
    player.on_sound_access = [&writes](const ntr::RegisterAccess& access)
    {
        writes += access.write ? 1 : 0;
    };
    std::vector<ntr::SoundOutput> out;
    int frames = 0;
    do
    {
        player.RunFrame(out);
        frames++;
    } while (player.Busy() && frames < 1000);
    XSFLIB_CHECK(frames >= 192 && frames < 200);
    XSFLIB_CHECK(writes > 0);
    XSFLIB_CHECK(Word16(player.MainMemory(), ntr::ArchivePlayer::kSharedWork + 0x264) == 7);
    XSFLIB_CHECK(player.Clock() == static_cast<uint64_t>(frames) * snd::kFrameClocks);

    // A frame holds 170 or 171 samples: 174,592 clocks over 1,024 a sample.
    XSFLIB_CHECK(out.size() == static_cast<uint64_t>(frames) * snd::kFrameClocks / 1024);
    bool sounded = false;
    for (const ntr::SoundOutput& sample : out)
    {
        sounded = sounded || sample.left != 0x200 || sample.right != 0x200;
    }
    XSFLIB_CHECK(sounded);
}

// The player stays busy while a channel releases after the sequence has ended. The sequence ends as its note does,
// after 96 frames. Release 100 lowers the envelope by 295 a frame, in units of 1/128 of 0.1 dB. The driver stops the
// channel at -72.3 dB, with the sequence's level of -2.1 dB in the sum: (723 - 21) * 128 / 295 gives 305 frames.
void TestBusyWhileReleasing()
{
    files::Archive spec = ArchiveSpec();
    spec.files[0] = files::Sequence({0x3C, 0x7F, 0x30, 0xFF});
    spec.files[2] = files::Bank({{.type = 1, .region = {.wave = 0, .release = 100}}});
    const Bytes file = files::SoundArchive(spec);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    int frames = 0;
    int ended = 0;
    do
    {
        player.RunFrame(out);
        frames++;
        if (ended == 0 && !player.Driver().GetPlayer(0).active)
        {
            ended = frames;
        }
    } while (player.Busy() && frames < 1000);
    XSFLIB_CHECK(ended >= 96 && ended < 100);
    XSFLIB_CHECK(frames >= ended + 304 && frames <= ended + 306);
}

void TestSnapshots()
{
    const Bytes file = ArchiveFile();
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    const ntr::ArchivePlayer::Snapshot unstarted = player.Save(nullptr);
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    for (int frame = 0; frame < 10; frame++)
    {
        player.RunFrame(out);
    }

    // The frames after a restore play as they did after the save, with a snapshot that shares pages with an earlier
    // one too.
    const ntr::ArchivePlayer::Snapshot first = player.Save(nullptr);
    player.RunFrame(out);
    const ntr::ArchivePlayer::Snapshot second = player.Save(&first);
    const std::vector<uint8_t> saved(player.MainMemory().begin(), player.MainMemory().end());
    std::vector<ntr::SoundOutput> played;
    for (int frame = 0; frame < 100; frame++)
    {
        player.RunFrame(played);
    }
    const uint64_t clock = player.Clock();

    player.Restore(second);
    XSFLIB_CHECK(std::equal(saved.begin(), saved.end(), player.MainMemory().begin()));
    std::vector<ntr::SoundOutput> again;
    for (int frame = 0; frame < 100; frame++)
    {
        player.RunFrame(again);
    }
    XSFLIB_CHECK(again == played && player.Clock() == clock);

    // A start sent before the save is taken after the restore.
    player.Restore(first);
    player.Send({{snd::CommandId::kGlobalVariable, {0, 9, 0, 0}}});
    const ntr::ArchivePlayer::Snapshot pending = player.Save(nullptr);
    player.RunFrame(out);
    player.Restore(pending);
    XSFLIB_CHECK(Word16(player.MainMemory(), ntr::ArchivePlayer::kSharedWork + 0x260) == 0xFFFF);
    player.RunFrame(out);
    XSFLIB_CHECK(Word16(player.MainMemory(), ntr::ArchivePlayer::kSharedWork + 0x260) == 9);

    // A restore takes the files back out of the heap. A start after it loads them where a new player does.
    player.Restore(unstarted);
    player.Start({0, 0});
    player.RunFrame(out);
    ntr::ArchivePlayer other(archive);
    other.Start({0, 0});
    std::vector<ntr::SoundOutput> other_out;
    other.RunFrame(other_out);
    XSFLIB_CHECK(std::ranges::equal(player.MainMemory(), other.MainMemory()));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { other.Restore(first); }) == "the snapshot is another player's");
}

void TestEnhanced()
{
    // The enhanced mode gives kEnhancedFrameSamples samples a frame in floating point. Its level follows the console's
    // within 0.5 dB on this wave at 32,728 Hz. A muted track gives 0, the meters follow the track, and a restore plays
    // the same samples again. A frame of the other mode's output is refused.
    const Bytes file = ArchiveFile(kTiedNotes, 100);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer console(archive);
    ntr::ArchivePlayerOptions options;
    options.enhanced = true;
    ntr::ArchivePlayer player(archive, options);
    player.MeterTracks(0);
    console.Start({0});
    player.Start({0});
    std::vector<ntr::SoundOutput> console_out;
    std::vector<float> out;
    double console_sum = 0.0;
    double sum = 0.0;
    bool metered = true;
    for (int frame = 0; frame < 100; frame++)
    {
        const std::size_t start = out.size();
        console.RunFrame(console_out);
        player.RunFrame(out);
        XSFLIB_CHECK(out.size() - start == 2 * ntr::kEnhancedFrameSamples);
        float peak = 0.0f;
        for (std::size_t i = start; i < out.size(); i += 2)
        {
            peak = std::max(peak, std::abs(out[i]));
        }
        const int32_t meter = player.TrackPeaks(0)[0];
        metered = metered && (frame < 2 || std::abs(meter - peak * 32768.0f) < 0.05f * peak * 32768.0f + 2.0f);
    }
    for (const ntr::SoundOutput& sample : console_out)
    {
        const double value = (sample.left - 0x200) * 64 / 32768.0;
        console_sum += value * value;
    }
    for (std::size_t i = 0; i < out.size(); i += 2)
    {
        sum += static_cast<double>(out[i]) * out[i];
    }
    const double console_rms = std::sqrt(console_sum / static_cast<double>(console_out.size()));
    const double rms = std::sqrt(sum / static_cast<double>(out.size() / 2));
    XSFLIB_CHECK(rms > 0.01);
    XSFLIB_CHECK(std::abs(20.0 * std::log10(rms / console_rms)) < 0.5);
    XSFLIB_CHECK(metered);
    XSFLIB_CHECK(player.TrackPeaks(1) == kNoPeaks);

    const ntr::ArchivePlayer::Snapshot snapshot = player.Save(nullptr);
    std::vector<float> played;
    for (int frame = 0; frame < 20; frame++)
    {
        player.RunFrame(played);
    }
    player.Restore(snapshot);
    std::vector<float> again;
    for (int frame = 0; frame < 20; frame++)
    {
        player.RunFrame(again);
    }
    XSFLIB_CHECK(again == played);

    ntr::ArchivePlayer muted(archive, options);
    muted.MuteTrack(0, 0, true);
    muted.Start({0});
    std::vector<float> silence;
    for (int frame = 0; frame < 100; frame++)
    {
        muted.RunFrame(silence);
    }
    XSFLIB_CHECK(std::ranges::all_of(silence, [](float sample) { return sample == 0.0f; }));

    XSFLIB_CHECK(ErrorOf<std::logic_error>([&] { player.RunFrame(console_out); }) ==
                 "the player mixes in the enhanced mode");
    XSFLIB_CHECK(ErrorOf<std::logic_error>([&] { console.RunFrame(out); }) ==
                 "the player doesn't mix in the enhanced mode");
}

} // namespace

int main()
{
    TestStartCommands();
    TestVariableCommands();
    TestReverbCommands();
    TestReverb();
    TestOutputModes();
    TestOutputEffectCommands();
    TestOutputEffect();
    TestPlay();
    TestBusyWhileReleasing();
    TestSnapshots();
    TestEnhanced();
    TestNotes();
    TestMutes();
    TestMeters();
    TestFramesWithoutMixing();
    TestHang();

    return xsflib::testing::Summary("DS archive player");
}
