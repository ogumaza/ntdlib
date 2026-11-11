// SPDX-License-Identifier: MIT

// A test of the model of DSE's driver (ntdlib/dse/driver.h). The test builds a song, its bank and a main bank that
// holds the bank's samples, plays the song on the model, and compares each tick's commands with those that the
// driver's code sends for the same files. dse_driver_test_lists.h holds a hash of the commands of each block of
// kBlockTicks ticks, and the channels' status that the driver read at each tick. Both come from the driver's code in
// Pokémon Mystery Dungeon: Explorers of Sky's ARM9 binary (North America). The game's code played the three files in
// place of the files of its first song, on ntdlib's ARM interpreter, with the game's ARM7 driver on Arm7System.
// dse_driver_test_earlier.h holds the same for a song with one more track, on the earlier build of the driver. It
// comes from Explorers of Time's ARM9 binary (North America), played the same way.
//
//     ntdlib_dse_driver_test [--write FOLDER]
//
// With --write, the test also writes its files into FOLDER: song.smd, bank.swd, main.swd, effects.sed and
// song_earlier.smd, the song of the test of the earlier build.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"
#include "xsflib/text.h"

#include "dse_driver_test_earlier.h"
#include "dse_driver_test_effects.h"
#include "dse_driver_test_lists.h"
#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/testing/dse_files.h"
#include "ntdlib/testing/dse_song_files.h"

namespace
{

namespace dse_files = ntdlib::testing::dse_files;
namespace dse_song_files = ntdlib::testing::dse_song_files;
namespace dse = ntdlib::dse;
namespace snd = ntdlib::ntr::snd;
using dse_files::Builder;
using dse_files::Bytes;
using dse_files::HeaderStart;
using dse_files::SequenceInfoBytes;
using dse_song_files::BankFile;
using dse_song_files::Effect;
using dse_song_files::Join;
using dse_song_files::kBankId;
using dse_song_files::MainBankFile;
using dse_song_files::Note;
using dse_song_files::SongFile;
using xsflib::testing::ErrorOf;

// The address at which the game's code placed the bank's samples.
constexpr uint32_t kSamples = 0x022D0040;

// The id of the test's bank of sound effects.
constexpr uint16_t kEffectsId = 1;

// The sound effects. Slot 0 is empty. Effect 1 ends. Effect 2 plays two tracks for ever. Effect 3 plays for ever, two
// instances at most. Effect 4 plays for ever, one instance at most, and refuses another. Effects 5 and 6 are of group
// 1, which plays one instance at most: effect 5 stops the oldest, and effect 6 refuses itself. Effects 7 and 8 play for
// ever at priorities 8 and 15. Effect 9 has five tracks, more than a sequence of the pool holds. Effect 10 plays the
// PSG and the noise for ever. Effect 11 plays notes with a change of tempo and ends. Effects 12 and 13 play for ever in
// group 2, which plays three instances at most for effect 12 and one for effect 13. Each stops the oldest. Effect 14's
// settings give two tracks, and it has one. Effect 15 ends, and any number of its instances play. With 16 slots, the
// table fills its 32 bytes, and the slot past it holds the first effect's settings.
std::vector<std::optional<Effect>> Effects()
{
    const Bytes select = {0xA9, 0x7F, 0xAA, 0x00};
    const auto looped = [&select](uint8_t number, uint8_t channel, uint8_t program, int key)
    {
        return Join({{number, channel, 0, 0},
                     select,
                     {0xAC, program, 0xE0, 0x70, 0xE8, 0x40, 0x99},
                     Note(0x60, key, 0, 0x18, 1),
                     {0x92, 0x18},
                     Note(0x50, key + 4, 0),
                     {0x92, 0x30, 0x98}});
    };
    const auto short_track = [&select](uint8_t number, uint8_t channel, int key)
    {
        return Join({{number, channel, 0, 0},
                     select,
                     {0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                     Note(0x7F, key, 0, 0x18, 1),
                     {0x92, 0x18, 0x98}});
    };
    return {
        std::nullopt,
        Effect{.tracks = {Join({{1, 0, 0, 0},
                                select,
                                {0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                                Note(0x60, 0, 0, 0x18, 1),
                                {0x92, 0x18},
                                Note(0x60, 4, 0),
                                {0x92, 0x18, 0x98}})}},
        Effect{.tracks = {looped(1, 0, 0, 2), looped(2, 1, 1, 5)}},
        Effect{.tracks = {looped(1, 0, 0, 7)}, .most = 2},
        Effect{.tracks = {looped(1, 0, 1, 9)}, .refuse = 1, .most = 1},
        Effect{.tracks = {looped(1, 0, 0, 11)}, .group = 1, .most = 1},
        Effect{.tracks = {looped(1, 0, 0, 1)}, .refuse = 1, .group = 1, .most = 1},
        Effect{.tracks = {looped(1, 0, 0, 3)}, .priority = 8},
        Effect{.tracks = {looped(1, 0, 1, 6)}, .priority = 15},
        Effect{.tracks = {short_track(1, 0, 0), short_track(2, 1, 2), short_track(3, 2, 4), short_track(4, 3, 5),
                          short_track(5, 3, 7)}},
        Effect{.tracks = {Join({{1, 0, 0, 0},
                                select,
                                {0xAC, 0x02, 0xE0, 0x60, 0xE8, 0x40, 0x99},
                                Note(0x60, 0, 0, 0x18, 1),
                                {0x92, 0x18},
                                Note(0x60, 0, 1),
                                {0x92, 0x18, 0xA0, 0x04, 0x98}})}},
        Effect{.tracks = {Join({{1, 0, 0, 0},
                                select,
                                {0xAC, 0x01, 0xE0, 0x7F, 0xE8, 0x40, 0xA4, 0x78},
                                Note(0x60, 0, 0, 0x18, 1),
                                {0x92, 0x18, 0xA4, 0xB4},
                                Note(0x60, 7, 0),
                                {0x92, 0x18},
                                Note(0x60, 4, 0),
                                {0x92, 0x30, 0x98}})}},
        Effect{.tracks = {looped(1, 0, 0, 8)}, .group = 2, .most = 3},
        Effect{.tracks = {looped(1, 0, 1, 10)}, .group = 2, .most = 1},
        Effect{.tracks = {short_track(1, 0, 12)}, .slots = 2},
        Effect{.tracks = {short_track(1, 0, 14)}, .most = 0},
    };
}

Bytes EffectsFile(const std::vector<std::optional<Effect>>& effects = Effects())
{
    return dse_song_files::EffectsFile(effects, kEffectsId);
}

// The song's tracks and a sixth, for the earlier build. It plays a key again while the key's first note sounds, and
// a note of length 0. It changes the octave inside a repeat, slides the volume and the pan in 0 ticks, and sets a
// volume above 127. It changes the program after the bend range, sets the LFO level, fades the song in a time of
// less than 256 ms, and holds a note past its end with the damper.
std::vector<Bytes> EarlierTracks()
{
    std::vector<Bytes> tracks = dse_song_files::Tracks();
    tracks.push_back(Join({{5, 5, 0, 0},
                           {0xA9, 0x7F, 0xAA, 0x00},
                           {0xAC, 0x00, 0xE0, 0xC8, 0xE8, 0x40, 0xDB, 0x0C, 0xAC, 0x00, 0xD7, 0x10, 0x00, 0xA0, 0x04},
                           Note(0x60, 0, 0, 0x30, 1),
                           {0x92, 0x0C},
                           Note(0x60, 0, 0, 0x0C, 1),
                           {0x92, 0x30, 0x9C, 0x03, 0xA1, 0x01},
                           Note(0x50, 0, 0, 0x08, 1),
                           {0x92, 0x08, 0x9D, 0xA0, 0x04},
                           Note(0x60, 4, 0, 0x00, 1),
                           {0x92, 0x18, 0xE2, 0x00, 0x00, 0x50, 0xEA, 0x00, 0x00, 0x20, 0xBE, 0x20},
                           {0x92, 0x18, 0xAF, 0x50, 0x00, 0x60, 0xBF, 0x7F},
                           Note(0x60, 7, 0, 0x0C, 1),
                           {0x92, 0x30, 0xBF, 0x00, 0x92, 0x60, 0x98}}));
    return tracks;
}

// FNV-1a over the bytes of `value`.
void Mix(uint64_t& hash, uint32_t value)
{
    for (int i = 0; i < 4; i++)
    {
        hash = (hash ^ ((value >> (8 * i)) & 0xFF)) * 0x100000001B3;
    }
}

// The hash of a tick's commands within its block: the tick's number, then each command's id and arguments.
void MixTick(uint64_t& hash, int tick, const snd::CommandList& list)
{
    Mix(hash, static_cast<uint32_t>(tick));
    for (const snd::Command& command : list)
    {
        Mix(hash, static_cast<uint32_t>(command.id));
        for (const uint32_t arg : command.args)
        {
            Mix(hash, arg);
        }
    }
}

void TestFilesRead()
{
    const dse::Song song = dse::Song::Parse(SongFile());
    XSFLIB_CHECK(song.tracks.size() == 5);
    XSFLIB_CHECK(song.Write() == SongFile());
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    XSFLIB_CHECK(bank.waves.size() == 6 && bank.instruments && bank.instruments->programs.size() == 128);
    XSFLIB_CHECK(bank.Write() == BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    XSFLIB_CHECK(main_bank.samples && main_bank.samples->size() == 0x400);
    XSFLIB_CHECK(main_bank.Write() == MainBankFile());
    const dse::SoundEffects effects = dse::SoundEffects::Parse(EffectsFile());
    XSFLIB_CHECK(effects.sequences.size() == 16 && !effects.sequences[0] && effects.sequences[9] &&
                 effects.sequences[9]->tracks.size() == 5);
    XSFLIB_CHECK(effects.Write() == EffectsFile());
}

// The waves that the main bank holds take (loop start + loop length) * 4 bytes each, in the order of the bank's slots.
// Wave 5 takes no room, and starts where wave 2 ends. The driver refuses a second bank with the same id.
void TestPlaceWaves()
{
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    dse::Driver driver;
    const std::optional<uint16_t> id = driver.AddBank(bank, 0);
    XSFLIB_CHECK(id == kBankId);
    XSFLIB_CHECK(!driver.AddBank(bank, 0));
    XSFLIB_CHECK(driver.PlaceWaves(kBankId, main_bank, kSamples) == (64 + 32 + 32) * 4);
}

// Without a song, every view of the followed sequence is empty.
void TestViewsWithoutSong()
{
    const dse::Driver driver;
    XSFLIB_CHECK(!driver.SequencePlays() && driver.SequenceTick() == 0 && driver.ChannelCount() == 0 &&
                 driver.TrackCount() == 0);
    XSFLIB_CHECK(driver.GetChannel(0) == dse::ChannelView{});
}

// Plays `driver` for each block of `hashes`, and checks each block's hash of the model's commands against the game's.
// Each tick takes the channels' status of `status`, and `act` runs before each tick with the tick's number.
template <std::size_t kBlocks, typename StatusChange, std::size_t kChanges, typename Act>
void CheckBlocks(dse::Driver& driver, const uint64_t (&hashes)[kBlocks], const StatusChange (&status)[kChanges],
                 int block_ticks, const Act& act)
{
    std::size_t change = 0;
    uint16_t now = 0;
    for (std::size_t block = 0; block < kBlocks; block++)
    {
        uint64_t hash = 0xCBF29CE484222325;
        for (int i = 0; i < block_ticks; i++)
        {
            const int tick = static_cast<int>(block) * block_ticks + i;
            while (change < kChanges && status[change].tick <= tick)
            {
                now = status[change++].status;
            }
            act(tick);
            MixTick(hash, i, driver.Tick(now));
        }
        const std::size_t first = block * static_cast<std::size_t>(block_ticks);
        if (!xsflib::testing::Check(hash == hashes[block], "the commands of ticks " + std::to_string(first) + " to " +
                                                               std::to_string(first + block_ticks - 1)))
        {
            break;
        }
    }
}

// Each block's hash of the model's commands matches the game's.
void TestSong()
{
    const dse::Song song = dse::Song::Parse(SongFile());
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    dse::Driver driver;
    driver.AddBank(bank, 0);
    driver.PlaceWaves(kBankId, main_bank, kSamples);
    driver.PlaySong(song);
    CheckBlocks(driver, dse_driver_test::kBlockHashes, dse_driver_test::kStatus, dse_driver_test::kBlockTicks,
                [](int) {});
    XSFLIB_CHECK(driver.Playing());
}

// The song with the earlier build's track on the earlier build. Each block's hash of the model's commands matches the
// game's, from Pokémon Mystery Dungeon: Explorers of Time's ARM9 binary (North America).
void TestEarlierSong()
{
    const dse::Song song = dse::Song::Parse(SongFile(EarlierTracks()));
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    dse::Driver driver(dse::Build::kEarlier);
    driver.AddBank(bank, 0);
    driver.PlaceWaves(kBankId, main_bank, kSamples);
    driver.PlaySong(song);
    CheckBlocks(driver, dse_driver_test_earlier::kBlockHashes, dse_driver_test_earlier::kStatus,
                dse_driver_test_earlier::kBlockTicks, [](int) {});
}

// The earlier build's count of a note's ticks in a made-up binary: a load of the word at 0x14C, a test, a branch, a
// subtraction of 1 and a store of the word. Another register in the store, or an empty binary, gives the later build.
void TestBuildOf()
{
    const auto binary = [](uint32_t store)
    {
        Builder b;
        b.U32(0xE1A00000).U32(0xE596014C).U32(0xE3500000).U32(0xDA000004).U32(0xE2500001).U32(store);
        b.U32(0xE1A00000);
        return b.Take();
    };
    XSFLIB_CHECK(dse::BuildOf(binary(0xE586014C)) == dse::Build::kEarlier);
    XSFLIB_CHECK(dse::BuildOf(binary(0xE587014C)) == dse::Build::kLater);
    XSFLIB_CHECK(dse::BuildOf({}) == dse::Build::kLater);
}

// A voice's release_ticks while the song plays: the ticks of a whole release while the voice sounds, then the rest of
// its release, a tick less in each tick, down to 0. The envelope steps once in the tick in which the voice releases.
void TestReleaseTicks()
{
    const dse::Song song = dse::Song::Parse(SongFile());
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    dse::Driver driver;
    driver.AddBank(bank, 0);
    driver.PlaceWaves(kBankId, main_bank, kSamples);
    driver.PlaySong(song);

    std::vector<dse::VoiceView> last(dse::Driver::kVoices);
    int releases = 0;
    int steps = 0;
    bool right = true;
    CheckBlocks(driver, dse_driver_test::kBlockHashes, dse_driver_test::kStatus, dse_driver_test::kBlockTicks,
                [&](int)
                {
                    for (int i = 0; i < dse::Driver::kVoices; i++)
                    {
                        const dse::VoiceView view = driver.GetVoice(i);
                        const dse::VoiceView& before = last[static_cast<std::size_t>(i)];
                        if (view.allocated && before.allocated && view.note == before.note && view.releasing &&
                            before.release_ticks)
                        {
                            const int32_t was = *before.release_ticks;
                            if (!before.releasing)
                            {
                                releases++;
                                right = right && view.release_ticks && *view.release_ticks == was - 1;
                            }
                            else
                            {
                                steps++;
                                right = right && view.release_ticks && *view.release_ticks == std::max(was - 1, 0);
                            }
                        }
                        last[static_cast<std::size_t>(i)] = view;
                    }
                });
    XSFLIB_CHECK(right && releases > 0 && steps > releases);
}

// Runs the actions of dse_driver_test_effects.h from `next` up to `tick`, and adds each effect that the driver refuses
// to `refused`, with the tick.
void RunActions(dse::Driver& driver, int tick, std::size_t& next, std::vector<std::pair<int, int>>& refused)
{
    namespace data = dse_driver_test_effects;
    for (; next < std::size(data::kActions) && data::kActions[next].tick <= tick; next++)
    {
        const data::Action& action = data::kActions[next];
        const auto value = [&action](int i)
        {
            return action.values[i];
        };
        const auto u16 = [&value](int i)
        {
            return static_cast<uint16_t>(value(i));
        };
        switch (action.kind)
        {
        case 0:
            {
                const dse::StartParameters parameters = {.flags = static_cast<uint8_t>(value(2)),
                                                         .delay_ms = static_cast<int16_t>(value(3)),
                                                         .tempo = static_cast<int16_t>(value(4)),
                                                         .tempo_ms = u16(5),
                                                         .bend = static_cast<int16_t>(value(6)),
                                                         .bend_ms = u16(7),
                                                         .volume = static_cast<int16_t>(value(8)),
                                                         .volume_ms = u16(9),
                                                         .pan = u16(10),
                                                         .pan_ms = u16(11)};
                if (!driver.PlayEffect(u16(0), u16(1), parameters, u16(12)))
                {
                    refused.emplace_back(tick, value(1));
                }
                break;
            }
        case 1:
            driver.StopSong(static_cast<uint32_t>(value(0)));
            break;
        case 2:
            driver.StopEffect(u16(0), u16(1), u16(2), static_cast<uint32_t>(value(3)));
            break;
        case 3:
            driver.StopHandle(u16(0), static_cast<uint32_t>(value(1)));
            break;
        default:
            driver.StopEffects(static_cast<uint32_t>(value(0)));
            break;
        }
    }
}

// The song with the sound effects that start and stop at the ticks of dse_driver_test_effects.h: steals of the pool's
// sequences, the limits of an effect's instances and of a group's, the priorities, the handles, every fade of a start,
// a start at a tempo of 0 and each kind of stop. Effect 13 stops all three instances of effect 12. Effect 14 then
// takes the sequence of effect 2, whose second track doesn't play again. Each block's hash of the model's commands
// matches the game's. The effects that the model refuses are those of slot 0, of slot 16 past the table, the second
// instance of effect 4, effect 6 while effect 5 plays, effect 8 while no sequence has its priority or more, effect 9
// with its five tracks, and effect 4 while effect 4 plays with the same handle.
//
// With `copy_tick`, the driver goes on from that tick as a copy of a copy of itself, and its first state is gone by
// then. The copy then plays as the driver does.
void TestEffects(std::optional<int> copy_tick = std::nullopt)
{
    namespace data = dse_driver_test_effects;
    const dse::Song song = dse::Song::Parse(SongFile());
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    dse::Driver driver;
    driver.AddBank(bank, 0);
    XSFLIB_CHECK(driver.AddEffects(dse::SoundEffects::Parse(EffectsFile())) == kEffectsId);
    XSFLIB_CHECK(!driver.AddEffects(dse::SoundEffects::Parse(EffectsFile())));
    driver.PlaceWaves(kBankId, main_bank, kSamples);
    driver.PlaySong(song);

    std::size_t next = 0;
    std::vector<std::pair<int, int>> refused;
    CheckBlocks(driver, data::kBlockHashes, data::kStatus, data::kBlockTicks,
                [&](int tick)
                {
                    if (tick == copy_tick)
                    {
                        std::vector<dse::VoiceView> voices;
                        std::vector<dse::ChannelView> channels;
                        for (int i = 0; i < dse::Driver::kVoices; i++)
                        {
                            voices.push_back(driver.GetVoice(i));
                        }
                        for (int i = 0; i < driver.ChannelCount(); i++)
                        {
                            channels.push_back(driver.GetChannel(i));
                        }
                        const dse::Driver copy(driver);
                        driver = dse::Driver();
                        driver = copy;
                        for (int i = 0; i < dse::Driver::kVoices; i++)
                        {
                            XSFLIB_CHECK(driver.GetVoice(i) == voices[static_cast<std::size_t>(i)]);
                        }
                        for (int i = 0; i < driver.ChannelCount(); i++)
                        {
                            XSFLIB_CHECK(driver.GetChannel(i) == channels[static_cast<std::size_t>(i)]);
                        }
                    }
                    RunActions(driver, tick, next, refused);
                });
    const std::vector<std::pair<int, int>> expected = {{82, 4},  {84, 0},  {85, 16}, {114, 6},
                                                       {134, 8}, {150, 9}, {262, 4}};
    XSFLIB_CHECK(refused == expected);
}

// The damper holds a note past its end, then goes off. The driver's code then breaks its lists of notes, and the game's
// code soon loops for ever. The model stops with an error.
void TestDamperOverEndedNote()
{
    const dse::Song song = dse::Song::Parse(SongFile({Join({{0, 0, 0, 0},
                                                            {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xBF, 0x7F},
                                                            Note(0x60, 0, 0, 0x0C, 1),
                                                            {0x92, 0x30, 0xBF, 0x00, 0x92, 0x30, 0x98}})}));
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    dse::Driver driver;
    driver.AddBank(bank, 0);
    driver.PlaceWaves(kBankId, main_bank, kSamples);
    driver.PlaySong(song);
    const std::string error = ErrorOf<std::runtime_error>(
        [&]
        {
            for (int i = 0; i < 500; i++)
            {
                driver.Tick(0);
            }
        });
    XSFLIB_CHECK(error.find("damper") != std::string::npos);
}

void Write(const std::filesystem::path& path, const Bytes& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
    {
        throw std::runtime_error("can't write " + path.string());
    }
}

} // namespace

// Track 1's loop holds its end and nothing else. The loop point takes one command, and each pass of the loop one more.
// The tick that starts the track stops at kCommandBudget commands, with the end as the next command. The file ends with
// the track's 6 bytes, 2 bytes of padding and the 16 bytes of the last chunk.
void TestHang()
{
    const Bytes file = SongFile({{0, 0, 0, 0, 0xA4, 0x78, 0x92, 0x30, 0x98}, {1, 1, 0, 0, 0x99, 0x98}});
    const dse::Song song = dse::Song::Parse(file);
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    dse::Driver driver;
    driver.AddBank(bank, 0);
    driver.PlaceWaves(kBankId, main_bank, kSamples);
    driver.PlaySong(song);
    std::optional<dse::Hang> hang;
    try
    {
        for (int tick = 0; tick < 100; tick++)
        {
            driver.Tick(0);
        }
    }
    catch (const dse::Hang& e)
    {
        hang = e;
    }
    XSFLIB_CHECK(hang && hang->TrackIndex() == 1 && hang->Offset() == file.size() - 16 - 8 + 5);
    XSFLIB_CHECK(hang &&
                 std::string(hang->what()) == "track 1 runs 100000 commands in a tick without waiting, up to offset " +
                                                  xsflib::Hex(static_cast<uint32_t>(file.size() - 19), 0, true) +
                                                  ": the sequence hangs");
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--write")
    {
        const std::filesystem::path folder = argv[2];
        Write(folder / "song.smd", SongFile());
        Write(folder / "bank.swd", BankFile());
        Write(folder / "main.swd", MainBankFile());
        Write(folder / "effects.sed", EffectsFile());
        Write(folder / "song_earlier.smd", SongFile(EarlierTracks()));
        return 0;
    }
    TestFilesRead();
    TestPlaceWaves();
    TestViewsWithoutSong();
    TestSong();
    TestEarlierSong();
    TestBuildOf();
    TestReleaseTicks();
    TestEffects();
    TestEffects(200);
    TestDamperOverEndedNote();
    TestHang();
    return xsflib::testing::Summary("dse_driver_test");
}
