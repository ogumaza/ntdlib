// SPDX-License-Identifier: MIT

// RSAR reader tests on small files built here. The files are a version 1.1 bank with a WAVE block and a version 1.2
// bank with an RWAR, with instruments that exercise every way the bank reader selects a region. Many instruments share
// references. The version 1.1 bank is also tested marked as version 1.0. The game ignores the regions' volumes in such
// a bank. There's also a sequence with labels, and an archive that holds them in a group, whole and truncated. Archives
// of the 3DS and later consoles, and little-endian files, must be rejected with a message.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/testing/rsar_files.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

namespace
{

using namespace xsflib;

using namespace ntdlib;
using namespace ntdlib::testing::rsar_files;
using xsflib::testing::Check;

// The rules of playback: the sentences, names as the archive has them, every field, and the sound type of an archive
// before version 1.1 from the sound table.
const rsar::ReadOptions kPlayback{rsar::Wording::kSentences, false, true, false};

// The original key of the region a note plays, or -1 for none.
int KeyOf(const rsar::Bank& bank, int program, int key, int velocity)
{
    const rsar::Region* region = bank.Find(program, key, velocity);

    return region ? region->original_key : -1;
}

void CheckInstruments(const std::string& name, const rsar::Bank& bank)
{
    Check(bank.InstrumentCount() == 70, name + ": instruments");
    Check(KeyOf(bank, 0, 10, 10) == 60 && KeyOf(bank, 0, 127, 127) == 60, name + ": a direct region");
    Check(KeyOf(bank, 1, 0, 64) == 40 && KeyOf(bank, 1, 63, 64) == 40 && KeyOf(bank, 1, 64, 64) == 80,
          name + ": a range table by key");
    Check(KeyOf(bank, 2, 59, 64) == -1 && KeyOf(bank, 2, 63, 64) == -1, name + ": an index table's bounds");
    Check(bank.Find(2, 61, 63)->wave_index == 0 && bank.Find(2, 61, 64)->wave_index == 1,
          name + ": an index table, then a range table by velocity");
    Check(!bank.Find(3, 60, 64) && !bank.Find(4, 60, 64) && !bank.Find(5, 60, 64),
          name + ": nothing, an unknown type and a negative wave index play nothing");
    Check(KeyOf(bank, 69, 64, 64) == 80 && !bank.Find(70, 60, 64) && !bank.Find(static_cast<uint32_t>(-1), 60, 64),
          name + ": shared references, and programs out of range");
}

void CheckWaves(const std::string& name, const rsar::Bank& bank, uint8_t first)
{
    Check(bank.Waves().size() == 2, name + ": the number of waves");
    for (size_t i = 0; i < bank.Waves().size() && i < 2; ++i)
    {
        const rsar::Wave& wave = bank.Waves()[i];
        Check(wave.encoding == rsar::WaveEncoding::kDspAdpcm && wave.loop && wave.sample_rate == 32000 &&
                  wave.loop_start_address == 2 && wave.loop_end_address == 30 && wave.channels.size() == 1,
              name + ": wave " + std::to_string(i) + "'s info");
        if (wave.channels.empty())
        {
            continue;
        }

        const rsar::WaveChannel& channel = wave.channels[0];
        Check(channel.adpcm->pred_scale == first + i && channel.data.size() >= 0x40 && channel.data[0] == first + i,
              name + ": wave " + std::to_string(i) + "'s samples and predictor/scale (" +
                  std::to_string(channel.adpcm->pred_scale) + ", " + std::to_string(channel.data.size()) +
                  " bytes, first " + std::to_string(channel.data.empty() ? -1 : channel.data[0]) + ")");
        Check(channel.adpcm->coefs[0] == 0x400 && channel.volumes[0] == 0x01000000,
              name + ": wave " + std::to_string(i) + "'s coefficients and volume (" +
                  std::to_string(channel.adpcm->coefs[0]) + ", " + std::to_string(channel.volumes[0]) + ")");
    }
}

// Checks that loading `bytes` as an archive with `options` fails with a message containing `words`.
void CheckRefused(const std::string& name, const Bytes& bytes, const std::string& words,
                  const rsar::ReadOptions& options = kPlayback)
{
    try
    {
        rsar::SoundArchive::Load(bytes, options);
        Check(false, name + ": loaded");
    }
    catch (const xsflib::FormatError& e)
    {
        Check(std::string(e.what()).find(words) != std::string::npos, name + ": the message \"" + e.what() + "\"");
    }
}

// The offset of the first copy of `text` in `bytes`.
size_t Find(const Bytes& bytes, const std::string& text)
{
    const auto at = std::search(bytes.begin(), bytes.end(), text.begin(), text.end());

    return static_cast<size_t>(at - bytes.begin());
}

// The banks of versions 1.1 and 1.2 under the rules of playback: their instruments and waves. A version 1.0 bank's
// regions take full volume.
void CheckBanks()
{
    // The banks' waves point into the bytes they're read from. Those bytes must outlive the banks.
    const Bytes old_file = MakeOldBank();
    const Bytes old_waves = MakeOldWaveData();
    const rsar::Bank old_bank = rsar::Bank::Parse(old_file, old_waves, kPlayback);
    CheckInstruments("version 1.1 bank", old_bank);
    CheckWaves("version 1.1 bank", old_bank, 0x21);

    // A bank before version 1.1 plays its regions at full volume, whatever volume they hold (100 here).
    Bytes first_file = MakeOldBank();
    xsflib::StoreBe16(&first_file[6], 0x0100);
    const rsar::Bank first_bank = rsar::Bank::Parse(first_file, old_waves, kPlayback);
    const rsar::Region* first_region = first_bank.Find(0, 60, 64);
    const rsar::Region* old_region = old_bank.Find(0, 60, 64);

    Check(first_region != nullptr && first_region->volume == 127 && first_region->tune == 1.0f,
          "a version 1.0 bank's regions take full volume");
    Check(old_region != nullptr && old_region->volume == 100, "a version 1.1 bank's regions take their volume");

    const Bytes new_file = MakeNewBank();
    const Bytes rwar = MakeRwar();
    const rsar::Bank new_bank = rsar::Bank::Parse(new_file, rwar, kPlayback);
    CheckInstruments("version 1.2 bank", new_bank);
    CheckWaves("version 1.2 bank", new_bank, 0x31);
}

// A sequence file of 16 bytes of commands, 0x2C to 0x3B, with labels at 0 and 8.
Bytes TestSequence()
{
    Bytes commands(16);
    for (size_t i = 0; i < commands.size(); ++i)
    {
        commands[i] = static_cast<uint8_t>(0x2C + i);
    }

    return MakeSequence(commands);
}

// Under the rules of playback: a sequence's commands and labels, and an archive that holds the sequence and a bank in
// a group, whole and cut short.
void CheckArchive()
{
    const Bytes sequence_bytes = TestSequence();
    const rsar::Sequence sequence = rsar::Sequence::Parse(sequence_bytes, kPlayback);
    Check(sequence.data.size() == 16 && sequence.labels.size() == 2 && sequence.labels[1].name == "SEQ_BB" &&
              sequence.labels[1].offset == 8,
          "a sequence's commands and labels");

    const Bytes bytes = MakeArchive(sequence_bytes, 8, 0);
    const rsar::SoundArchive archive = rsar::SoundArchive::Load(bytes, kPlayback);
    Check(archive.Version() == 0x0104 && archive.Sounds().size() == 1 && archive.Banks().size() == 1 &&
              !archive.Truncated(),
          "an archive's tables");
    const rsar::SoundInfo& sound = archive.Sounds()[0];
    Check(sound.name == "SEQ_TEST" && sound.type == rsar::SoundType::kSequence && sound.volume == 90 &&
              sound.pan_mode == 0 && sound.pan_curve == 3 && sound.start_offset == 8 && sound.channel_priority == 70 &&
              sound.release_priority_fix && sound.bank == 0,
          "a sequence sound's info");
    Check(archive.Banks()[0].name == "BANK_TEST" && archive.FindSound("SEQ_TEST") == 0u &&
              !archive.FindSound("SEQ_OTHER"),
          "names");
    const auto sequence_file = archive.File(0);
    const auto bank_file = archive.File(1);
    Check(sequence_file && rsar::Sequence::Parse(sequence_file->file, kPlayback).labels.size() == 2 &&
              sequence_file->wave_data.empty(),
          "the sequence from the group");
    Check(bank_file && rsar::Bank::Parse(bank_file->file, bank_file->wave_data, kPlayback).Waves().size() == 2,
          "the bank and its RWAR from the group");
    const std::span<const uint8_t> all = archive.Bytes();
    const auto inside = [&all](std::span<const uint8_t> data)
    {
        return data.data() >= all.data() && data.data() + data.size() <= all.data() + all.size();
    };
    Check(std::equal(all.begin(), all.end(), bytes.begin(), bytes.end()) && sequence_file &&
              inside(sequence_file->file) && bank_file && inside(bank_file->file) && inside(bank_file->wave_data),
          "the files lie in the archive's bytes");

    // Cut short in the bank's wave data. The sequence is still there. The bank isn't.
    const Bytes cut(bytes.begin(), bytes.end() - 100);
    const rsar::SoundArchive truncated = rsar::SoundArchive::Load(cut, kPlayback);
    Check(truncated.Truncated() && truncated.File(0) && !truncated.File(1), "a truncated archive");
}

// An archive's tables of sounds and players.
void CheckSoundTable()
{
    const Bytes table_bytes = MakeSoundTable({{"SEQ_FIELD", 1, 1}, {"STRM_TITLE", 2, 0}}, {"PLAYER_BGM", "PLAYER_SE"});
    const rsar::SoundArchive table = rsar::SoundArchive::Load(table_bytes, kPlayback);
    Check(table.Players().size() == 2 && table.Players()[0].name == "PLAYER_BGM" &&
              table.Players()[1].name == "PLAYER_SE" && table.Players()[1].sound_count == 2,
          "an archive's players");
    Check(table.Sounds().size() == 2 && table.Sounds()[0].name == "SEQ_FIELD" && table.Sounds()[0].player_id == 1 &&
              table.Sounds()[1].type == rsar::SoundType::kStream,
          "the sounds' players");
}

// Archives of the 3DS and later consoles, a little-endian archive and a file that isn't an archive are refused with a
// message.
void CheckOtherFormats()
{
    const Bytes bytes = MakeArchive(TestSequence(), 8, 0);
    Bytes ctr = bytes;
    std::memcpy(ctr.data(), "CSAR", 4);
    CheckRefused("a 3DS archive", ctr, "3DS");
    std::memcpy(ctr.data(), "FSAR", 4);
    CheckRefused("a Wii U or Switch archive", ctr, "Wii U or Switch");
    Bytes little = bytes;
    xsflib::StoreBe16(&little[4], 0xFFFE);
    CheckRefused("a little-endian archive", little, "big-endian");
    CheckRefused("something else", Bytes(64, 'x'), "RSAR");
}

// The default rules: the short texts, printable names that are made up where they're empty, no field that only playback
// uses, the loop in samples, waves read one at a time, and files found without their wave data.
void CheckDefaultRules()
{
    const Bytes old_file = MakeOldBank();
    const Bytes old_waves = MakeOldWaveData();
    const rsar::Bank bank = rsar::Bank::Parse(old_file);
    const rsar::Wave wave = bank.ReadWave(1, old_waves);
    Check(bank.HasWaveBlock() && bank.Waves().empty(), "defaults: a version 1.1 bank reads its waves one at a time");
    Check(wave.loop_start == 0 && wave.loop_end == 27 && wave.loop_start_address == 2 && wave.loop_end_address == 30,
          "defaults: the loop in samples and in DSP addresses");
    Check(wave.channels.size() == 1 && wave.channels[0].adpcm && wave.channels[0].adpcm->coefs[0] == 0x400 &&
              wave.channels[0].adpcm->pred_scale == 0 && wave.channels[0].volumes[0] == 0,
          "defaults: a channel's coefficients, and no playback fields");

    Bytes commands(16, 0);
    const Bytes sequence_bytes = MakeSequence(commands);
    Bytes bytes = MakeArchive(sequence_bytes, 8, 0);
    bytes[Find(bytes, "SEQ_TEST") + 3] = 0x07;
    bytes[Find(bytes, "BANK_TEST")] = 0;
    const rsar::SoundArchive archive = rsar::SoundArchive::Load(bytes);
    const rsar::SoundArchive played = rsar::SoundArchive::Load(bytes, kPlayback);
    const rsar::SoundInfo& sound = archive.Sounds()[0];
    Check(sound.name == "SEQ?TEST" && archive.Banks()[0].name == "BANK_0" && played.Sounds()[0].name == "SEQ\aTEST" &&
              played.Banks()[0].name.empty(),
          "defaults: printable names, and a made-up name for an empty one");
    Check(sound.start_offset == 8 && sound.bank == 0 && sound.player_id == 0 && sound.channel_priority == 64 &&
              archive.Players().empty() && archive.Groups().empty() && archive.InfoSize() == 0,
          "defaults: a sequence sound's info, without playback fields");
    Check(!archive.FileData(0).empty() && archive.WaveData(0).empty() && !archive.FileData(1).empty() &&
              !archive.WaveData(1).empty() && rsar::Sequence::Parse(archive.FileData(0)).labels.empty(),
          "defaults: the files from the group");

    // Cut short in the bank's wave data. The default rules still find the bank, without its wave data.
    const Bytes cut(bytes.begin(), bytes.end() - 100);
    const rsar::SoundArchive truncated = rsar::SoundArchive::Load(cut);
    Check(truncated.Truncated() && !truncated.FileData(1).empty() && truncated.WaveData(1).empty() &&
              truncated.WaveDataMissing(1) && !truncated.FileMissing(1),
          "defaults: a truncated archive");

    // The short texts, and the sentences for the same files.
    Bytes ctr = bytes;
    std::memcpy(ctr.data(), "SDAT", 4);
    CheckRefused("defaults: a DS archive", ctr, "unsupported SDAT archive (DS); expected RSAR (Wii)", {});
    CheckRefused("playback: a DS archive", ctr, "not an RSAR file");
    Bytes little = bytes;
    xsflib::StoreBe16(&little[4], 0xFFFE);
    CheckRefused("defaults: a little-endian archive", little, "unsupported byte order", {});
    CheckRefused("defaults: something else", Bytes(64, 'x'), "bad magic, expected RSAR", {});
    CheckRefused("defaults: a short file", Bytes(bytes.begin(), bytes.begin() + 8), "too short to be an RSAR file", {});
    const Bytes header(bytes.begin(), bytes.begin() + 0x20);
    CheckRefused("defaults: a header alone", header, "truncated archive: 32 of ", {});
    CheckRefused("playback: a header alone", header, "the sound archive has no INFO block");
}

// Waves decode to PCM. The Wii's PCM16 is big-endian. NW4R SND plays only the first two channels.
void CheckDecoding()
{
    const rsar::Pcm pcm = rsar::DecodeWave(rsar::Wave::ParseFile(MakePcm16Rwav(4, {1, -2, 300, -32768})));
    const rsar::Pcm two = rsar::DecodeWave(rsar::Wave::ParseFile(MakePcm16Rwav(4, {1, -2, 300, -32768}, 3)));
    const Bytes longer = MakePcm16Rwav(0xffffffff, {1, 2, 3, 4});
    const auto decode_longer = [&longer]()
    {
        rsar::DecodeWave(rsar::Wave::ParseFile(longer));
    };

    Check(pcm.channels.size() == 1 && !pcm.loop && pcm.Length() == 4 && pcm.sample_rate == 32000 &&
              pcm.channels[0] == std::vector<int16_t>({1, -2, 300, -32768}),
          "a PCM16 wave decodes");
    Check(two.channels.size() == 2 && two.channels[1] == std::vector<int16_t>({1, -2, 300, -32768}),
          "a wave decodes its first two channels");
    Check(xsflib::testing::ErrorOf(decode_longer) == "wave data shorter than the declared sample count",
          "a wave longer than its data is reported");
}

} // namespace

int main()
{
    try
    {
        CheckBanks();
        CheckArchive();
        CheckSoundTable();
        CheckOtherFormats();
        CheckDefaultRules();
        CheckDecoding();
    }
    catch (const std::exception& e)
    {
        Check(false, std::string("unexpected error: ") + e.what());
    }

    return xsflib::testing::Summary("rsar");
}
