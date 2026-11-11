// SPDX-License-Identifier: MIT

// Unit tests for MP2K's archive player: its FIFOs, stores, snapshots, mutes, meters, notes, memory, end and errors, and
// its archive mode. They build small synthetic cartridges in memory. No game ROM is needed.

#include "mp2k_test_util.h"

namespace ntdlib::sequence_export::mp2k
{
namespace
{

void TestPlayerFifo()
{
    // The mixer's points of each frame play in the next frame, after the kFifoLead points that the FIFOs hold ahead.
    // Timer 0 overflows every 280,896 / 224 = 1,254 cycles, from the start. Each output sample plays the point of the
    // last overflow at or before it. FIFO A plays the right side, and FIFO B the left. A point p adds 4p to the
    // output's 10 bits, at the FIFOs' volume of 100%: 256p in a 16-bit sample. The PSG stays silent.
    Cart cart;
    const std::vector<int8_t> pcm = Jumpy(300);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(pcm))}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x20).Note(24, 60, 127).Wait(48).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    constexpr int kFrames = 12;

    Sequencer seq(rom, info, 0);
    const MixedFrames m = Mix(seq, kFrames);
    SEQLIB_REQUIRE(FirstSound(m) >= 0);
    std::vector<int8_t> right(size_t(224 + kFifoLead), 0);
    std::vector<int8_t> left(size_t(224 + kFifoLead), 0);
    for (int f = 0; f < kFrames; f++)
    {
        right.insert(right.end(), m.right[size_t(f)].begin(), m.right[size_t(f)].end());
        left.insert(left.end(), m.left[size_t(f)].begin(), m.left[size_t(f)].end());
    }

    ArchivePlayer player(rom, info);
    player.Start(0);
    SEQLIB_REQUIRE_EQ(player.Rate(), 65536);
    const std::vector<int16_t> out = Play(player, kFrames);
    SEQLIB_REQUIRE_EQ(out.size(), size_t(2 * kFrames * kFrameCycles / kOutputCycles));
    bool matches = true;
    bool sounds = false;
    for (size_t k = 0; k < out.size() / 2; k++)
    {
        const size_t pop = k * kOutputCycles / (kFrameCycles / 224);
        matches &= out[2 * k] == 256 * left[pop] && out[2 * k + 1] == 256 * right[pop];
        sounds |= out[2 * k] != 0;
    }
    SEQLIB_CHECK(matches);
    SEQLIB_CHECK(sounds);
    SEQLIB_CHECK_EQ(player.Frames(), uint64_t(kFrames));
}

void TestPlayerStoreCycle()
{
    // The driver's writes take effect at the options' cycle of each frame. The output stays silent before the cycle
    // of the frame whose writes first give square 1 a level in NR12 (0x04000063). A square at key 60, about 262 Hz,
    // sounds within a cycle of its wave after it: 64,000 of the CPU's cycles.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(48, 60, 127).Wait(48).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 0);
    uint32_t level_frame = 0;
    bool level = false;
    while (!level && level_frame < 10)
    {
        seq.Step();
        for (const ntdlib::agb::RegisterWrite& w : seq.SoundWrites())
        {
            level |= w.address == 0x04000063 && (w.value >> 4) > 0;
        }
        level_frame += level ? 0 : 1;
    }
    SEQLIB_REQUIRE(level);
    for (uint32_t cycle : {0u, 100000u, 280000u})
    {
        ArchivePlayer player(rom, info);
        ArchivePlayerOptions options;
        options.store_cycle = cycle;
        player.Start(0, options);
        const std::vector<int16_t> out = Play(player, 3);
        size_t first = 0;
        while (first < out.size() && out[first] == 0)
        {
            first++;
        }
        SEQLIB_REQUIRE(first < out.size());
        const uint64_t at = first / 2 * kOutputCycles;
        const uint64_t stores = uint64_t(level_frame) * kFrameCycles + cycle;
        SEQLIB_CHECK(at >= stores && at < stores + 64000);
    }
}

void TestPlayerFramesWithoutSamples()
{
    // Frames without samples leave the player as frames with them do. The song's reverb keeps the mixer's buffer in
    // the state.
    const TwoTracks songs = TwoTrackSongs(0x80 | 60);
    ArchivePlayer reference(songs.rom, songs.info);
    reference.Start(0);
    const std::vector<std::vector<int16_t>> expected = PlayFrames(reference, 120);

    ArchivePlayer player(songs.rom, songs.info);
    player.MeterTracks(true);
    player.Start(0);
    PlayFrames(player, 30);
    bool silent_meters = true;
    for (int f = 0; f < 40; f++)
    {
        player.RunFrame();
        silent_meters &=
            player.TrackPeaks(0) == std::array<int32_t, 2>{} && player.TrackPeaks(1) == std::array<int32_t, 2>{};
    }
    SEQLIB_CHECK(silent_meters);
    const std::vector<std::vector<int16_t>> after = PlayFrames(player, 50);
    SEQLIB_CHECK(std::equal(after.begin(), after.end(), expected.begin() + 70));
    SEQLIB_CHECK_EQ(player.Frames(), 120u);
    SEQLIB_CHECK_EQ(player.Busy(), reference.Busy());
}

void TestPlayerSnapshots()
{
    // A restore gives the frames after the snapshot again. The mutes and the meters stay through it.
    const TwoTracks songs = TwoTrackSongs(0x80 | 60);
    ArchivePlayer player(songs.rom, songs.info);
    SEQLIB_CHECK(Throws<std::logic_error>([&] { player.Save(); }));
    player.Start(0);
    PlayFrames(player, 30);
    const ArchivePlayer::Snapshot snapshot = player.Save();
    const std::vector<int16_t> first = Play(player, 60);
    player.MuteTrack(1, true);
    player.MeterTracks(true);
    player.Restore(snapshot);
    SEQLIB_CHECK_EQ(player.Frames(), 30u);
    SEQLIB_CHECK(player.IsTrackMuted(1));
    player.MuteTrack(1, false);
    std::vector<int16_t> second;
    bool metered = false;
    for (int f = 0; f < 60; f++)
    {
        player.RunFrame(second);
        metered |= player.TrackPeaks(0)[0] > 0;
    }
    SEQLIB_CHECK(first == second);
    SEQLIB_CHECK(metered);

    ArchivePlayer other(songs.rom, songs.info);
    other.Start(0);
    SEQLIB_CHECK(Throws<std::invalid_argument>([&] { other.Restore(snapshot); }));
}

void TestPlayerMutes()
{
    // A muted track plays as a rest in its place would, whether its channel is a DirectSound channel or a square.
    const TwoTracks songs = TwoTrackSongs();
    const auto play = [&](int song, int muted)
    {
        ArchivePlayer player(songs.rom, songs.info);
        if (muted >= 0)
        {
            player.MuteTrack(muted, true);
        }
        player.Start(song);
        return Play(player, 120);
    };
    const std::vector<int16_t> both = play(0, -1);
    const std::vector<int16_t> square = play(1, -1);
    const std::vector<int16_t> saw = play(2, -1);
    SEQLIB_CHECK(both != square && both != saw && Peak(square, 0) > 0 && Peak(saw, 0) > 0);
    SEQLIB_CHECK(play(0, 0) == square);
    SEQLIB_CHECK(play(0, 1) == saw);

    ArchivePlayer player(songs.rom, songs.info);
    SEQLIB_CHECK(Throws<std::out_of_range>([&] { player.MuteTrack(16, true); }));
    SEQLIB_CHECK(Throws<std::out_of_range>([&] { player.MuteTrack(-1, true); }));
}

void TestPlayerMeters()
{
    // A track's meter gives the largest part of the output that its channels give. With the other track muted, the
    // track's largest meter over the song is the output's largest sample. Each side's output then takes the track's
    // channels alone: a FIFO's point times 256, or a square's level times 2 times NR50's master volume plus 1, times
    // 64. Neither loses bits to the 8 bits of the output's resolution. A muted track's meter gives 0, and so does any
    // track's without meters.
    const TwoTracks songs = TwoTrackSongs();
    for (int track = 0; track < 2; track++)
    {
        ArchivePlayer player(songs.rom, songs.info);
        player.MuteTrack(1 - track, true);
        player.MeterTracks(true);
        player.Start(0);
        std::vector<int16_t> out;
        std::array<int32_t, 2> peaks = {};
        bool muted_silent = true;
        for (int f = 0; f < 120; f++)
        {
            player.RunFrame(out);
            for (size_t side = 0; side < 2; side++)
            {
                peaks[side] = std::max(peaks[side], player.TrackPeaks(track)[side]);
            }
            muted_silent &= player.TrackPeaks(1 - track) == std::array<int32_t, 2>{};
        }
        SEQLIB_CHECK(peaks[0] > 0 && peaks[0] == Peak(out, 0));
        SEQLIB_CHECK(peaks[1] > 0 && peaks[1] == Peak(out, 1));
        SEQLIB_CHECK(muted_silent);
    }

    ArchivePlayer player(songs.rom, songs.info);
    player.Start(0);
    bool silent = true;
    for (int f = 0; f < 60; f++)
    {
        std::vector<int16_t> out;
        player.RunFrame(out);
        silent &= player.TrackPeaks(0) == std::array<int32_t, 2>{} && player.TrackPeaks(1) == std::array<int32_t, 2>{};
    }
    SEQLIB_CHECK(silent);
}

void TestPlayerNotes()
{
    // on_note gives each note that starts, with its first byte: the note's command, or its key where it repeats the
    // command.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const size_t first = track.Here();
    track.Note(24, 60, 100).Wait(24);
    const size_t second = track.Here();
    track.Raw({62}).Wait(24).Fine();
    const uint32_t header = cart.Song({track}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const uint32_t start = rom.U32(header + 8);

    ArchivePlayer player(rom, info);
    std::vector<std::pair<uint32_t, int>> notes;
    player.on_note = [&](const Action& a)
    {
        notes.emplace_back(a.command, a.key);
    };
    player.Start(0);
    for (int f = 0; f < 80; f++)
    {
        player.RunFrame();
    }
    const std::vector<std::pair<uint32_t, int>> expected = {{start + uint32_t(first), 60},
                                                            {start + uint32_t(second), 62}};
    SEQLIB_CHECK(notes == expected);
}

void TestPlayerMemory()
{
    // A memacc command that jumps when byte 3 of the memory holds 1 reaches a note. The memory starts at 0. A write
    // before the first frame reaches the command.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const size_t jump = track.Here();
    track.MemAccJump(6, 3, 1, jump + 9).Fine().Note(24, 60, 100).Wait(24).Fine();
    cart.Song({track}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    for (int value : {-1, 1, 2})
    {
        ArchivePlayer player(rom, info);
        int notes = 0;
        player.on_note = [&](const Action&)
        {
            notes++;
        };
        player.Start(0);
        if (value >= 0)
        {
            player.WriteMemory(3, uint8_t(value));
        }
        for (int f = 0; f < 30; f++)
        {
            player.RunFrame();
        }
        SEQLIB_CHECK_EQ(notes, value == 1 ? 1 : 0);
        SEQLIB_CHECK_EQ(int(player.Driver().Memory(3)), std::max(value, 0));
    }

    ArchivePlayer player(rom, info);
    SEQLIB_CHECK(Throws<std::out_of_range>([&] { player.WriteMemory(256, 0); }));
    SEQLIB_CHECK(Throws<std::out_of_range>([&] { player.WriteMemory(-1, 0); }));
}

void TestPlayerEnd()
{
    // A looped sample plays until its note's release of 0 stops it. The track ends in the same tick. Busy() stays true
    // until the FIFOs have played the mixer's last points: the frame after the mixer's last sound, and the next. The
    // output after it is silent. A reverb keeps the song busy while it echoes, and until the whole buffer holds 0: up
    // to a pass of the buffer's 7 parts later.
    std::array<int, 2> ends = {};
    for (int r = 0; r < 2; r++)
    {
        Cart cart;
        const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(Jumpy(224), 13379, 0))}});
        cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 60, 127).Wait(24).Fine()}, voices, 0, 0,
                  r ? 0x80 | 100 : 0);
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        ArchivePlayer player(rom, info);
        SEQLIB_CHECK(!player.Busy());
        player.Start(0);
        std::vector<std::vector<int16_t>> frames;
        while (player.Busy() && frames.size() < 600)
        {
            frames.emplace_back();
            player.RunFrame(frames.back());
        }
        SEQLIB_REQUIRE(!player.Busy());
        const auto loud = [](const std::vector<int16_t>& frame)
        {
            return std::any_of(frame.begin(), frame.end(), [](int16_t s) { return s != 0; });
        };
        size_t last = frames.size();
        while (last > 0 && !loud(frames[last - 1]))
        {
            last--;
        }
        SEQLIB_CHECK(last > 0 && last + (r ? 8 : 1) >= frames.size());
        SEQLIB_CHECK(!loud(Play(player, 10)));
        ends[size_t(r)] = int(frames.size());
    }
    SEQLIB_CHECK(ends[1] > ends[0] + 7);
}

void TestPlayerErrors()
{
    // The player refuses a song that can't be played, options out of range, a frame before a start, and Camelot's
    // mixer whose echo doesn't reach back within a frame.
    const TwoTracks songs = TwoTrackSongs();
    ArchivePlayer player(songs.rom, songs.info);
    SEQLIB_CHECK(Throws<std::logic_error>([&] { player.RunFrame(); }));
    SEQLIB_CHECK(Throws<std::logic_error>([&] { player.Driver(); }));
    SEQLIB_CHECK(Throws<std::invalid_argument>([&] { player.Start(5); }));
    for (int option = 0; option < 3; option++)
    {
        ArchivePlayerOptions options;
        options.store_cycle = option == 0 ? kFrameCycles : 0;
        options.envelope_counter = option == 1 ? 15 : 0;
        options.sequencer_step = option == 2 ? 8 : 0;
        SEQLIB_CHECK(Throws<std::invalid_argument>([&] { player.Start(0, options); }));
    }

    DriverInfo camelot = songs.info;
    camelot.camelot_mixer = true;
    camelot.camelot_tap = camelot.samples_per_frame;
    ArchivePlayer camelot_player(songs.rom, camelot);
    SEQLIB_CHECK(Throws<std::invalid_argument>([&] { camelot_player.Start(0); }));
}

// A test program's set for MP2K's archive mode: its files by path, and the song that it plays.
struct RomSet
{
    std::vector<uint8_t>* File(const std::string& path)
    {
        const auto file = files.find(path);
        return file != files.end() ? &file->second : nullptr;
    }

    std::map<std::string, std::vector<uint8_t>> files;
    int song = 0;
};

struct RomProgram
{
    using Set = RomSet;

    static std::optional<std::string> Select(RomSet& set, ntdlib::mp2k::ArchiveSelection& selection)
    {
        selection.rom_path = "game.gba";
        selection.song = set.song;
        return std::nullopt;
    }
};

using RomSource = xsflib::player::ArchiveSource<ntdlib::mp2k::ArchiveTraits<RomProgram>>;

// Returns a set that holds `rom` as game.gba, with song `song` selected.
RomSet SetOf(const Rom& rom, int song)
{
    RomSet set;
    set.files["game.gba"].assign(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());
    set.song = song;
    return set;
}

void TestPlayerArchiveSource()
{
    // Archive mode plays the song as the player does, takes snapshots, and gives the player's refusals as its errors. A
    // ROM without the driver fails to load.
    const TwoTracks songs = TwoTrackSongs(0x80 | 60);
    ArchivePlayer player(songs.rom, songs.info);
    player.Start(0);
    const std::vector<int16_t> expected = Play(player, 120);

    RomSource source;
    RomSet set = SetOf(songs.rom, 0);
    std::vector<int16_t> out;
    SEQLIB_REQUIRE(!source.Start(set, out));
    SEQLIB_CHECK(out.empty() && source.Busy());
    for (int f = 0; f < 60; f++)
    {
        SEQLIB_CHECK(source.RunFrame(out));
    }
    const std::shared_ptr<ArchivePlayer::Snapshot> snapshot = source.Save(nullptr);
    for (int f = 0; f < 60; f++)
    {
        source.RunFrame(out);
    }
    SEQLIB_CHECK(out == expected);
    source.Restore(*snapshot);
    std::vector<int16_t> again;
    for (int f = 0; f < 60; f++)
    {
        source.RunFrame(again);
    }
    SEQLIB_CHECK(std::equal(again.begin(), again.end(), expected.end() - long(again.size())));

    RomSet missing = SetOf(songs.rom, 5);
    const std::optional<std::string> refused = source.Start(missing, out);
    SEQLIB_CHECK(refused && refused->find("song 5") != std::string::npos);
    RomSet empty;
    const std::optional<std::string> absent = source.Start(empty, out);
    SEQLIB_CHECK(absent && absent->find("game.gba") != std::string::npos);
    RomSet blank = SetOf(Cart().ToRom(), 0);
    SEQLIB_CHECK(Throws<xsflib::FormatError>([&] { source.Start(blank, out); }));
}

// The enhanced mode's samples in each frame.

int RunTests()
{
    TestPlayerFifo();
    TestPlayerStoreCycle();
    TestPlayerFramesWithoutSamples();
    TestPlayerSnapshots();
    TestPlayerMutes();
    TestPlayerMeters();
    TestPlayerNotes();
    TestPlayerMemory();
    TestPlayerEnd();
    TestPlayerErrors();
    TestPlayerArchiveSource();

    return seqlib::testing::Summary("mp2k_player");
}

} // namespace
} // namespace ntdlib::sequence_export::mp2k

int main()
{
    return ntdlib::sequence_export::mp2k::RunTests();
}
