// SPDX-License-Identifier: MIT

// A model of Nintendo SPD's sound driver for the DS: its 16 players, its channels, and the sound unit's registers that
// it writes in each frame.
//
// The driver runs on the ARM7. The ARM9 loads the sound data into main memory and sends the driver lists of messages:
// start a sound, stop it, fade it, change a player's volume, pitch, tempo or pan, set the channel groups, or set the
// capture reverb. A sound's flags name the player that plays it, and a player plays one sound at a time. A sound's
// start and end can send another message from its block's table of actions, such as the start of another sound.
//
// Each frame (performance.h gives its length) the driver takes the messages, runs the fades and the tracks of each
// player in turn, and then updates each channel: its envelope, its pitch and its pan. It writes each channel's
// volume, timer and pan, starts the channels of new notes, and clears the registers of every free channel.
//
// The model follows the driver's code in Super Princess Peach (USA) on the ARM7: the frame at 0x037FA908, the
// messages at 0x037FA6DC, a sound's start at 0x037F8D14, the tracks at 0x037F9328, the channels' update at
// 0x037F90D8, the envelope at 0x037F9AAC, the pitch at 0x037F9C70 and the registers at 0x037FA1F8. It keeps the
// driver's quirks, such as the release that starts from the sustain level.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "ntdlib/spd/song.h"

namespace ntdlib::spd
{

// The length of a frame: 4,096 ticks of the OS timer. The OS timer counts 33,513,982 / 64 Hz.
constexpr double kFrameSeconds = 4096.0 * 64.0 / 33513982.0;

// The frames of a tick at tempo t are kTickFrames / t.
constexpr int kTickFrames = 160;

constexpr int kPlayers = 16;
constexpr int kChannels = 16;

// The kinds of a message, from its first byte. Byte 1 holds a length of a fade or a value, and the upper halfword
// a sound and its set (sound << 6 | set) or a mask of players.
enum class MessageKind : uint8_t
{
    kNothing = 1,
    kStart = 2,
    kStop = 3,
    kPause = 4,
    kResume = 5,
    kRestartPlayers = 6,
    kStopPlayers = 7,
    kPausePlayers = 8,
    kResumePlayers = 9,
    kLevel = 10,
    kVolume = 11,
    kPitch = 12,
    kTempo = 13,
    kPan = 14,
    kChannelGroups = 15,
    kReverb = 16,
};

// The message that starts sound `sound` (from 1) of the block in slot `set`, or of set A when `set` is 0. `fade` gives
// the frames of a fade-in, or 0.
uint32_t StartMessage(int set, int sound, int fade = 0);

// Where the game keeps the sound data in main memory. The driver gives the hardware the samples' addresses, and its
// capture reverb writes into the sound area.
struct Layout
{
    uint32_t area = 0;  // the header's first 1,024 bytes, then set A, then set B
    uint32_t set_a = 0; // set A's block
    uint32_t set_b = 0; // set B's block
};

// Super Princess Peach's layout: the area at 0x0214A140, set A at the offset that the header gives slot 0, and set B
// right after set A. The game's start-up sends the area's address to the driver in command 1, and 0x10000 in command
// 2. A boot of the game on ntdlib's DS sends both within 0.11 s.
Layout GameLayout(std::span<const uint8_t> header);

// A write to the sound unit's registers.
struct RegisterWrite
{
    uint32_t address = 0;
    uint8_t size = 0;
    uint32_t value = 0;

    friend bool operator==(const RegisterWrite&, const RegisterWrite&) = default;
};

// A channel's state, as the driver keeps it in its 32 bytes.
struct Channel
{
    uint32_t sample = 0; // the address of the sample's header, from the note's start
    int player = -1;     // the player and the track that play the note
    int track = 0;
    uint8_t volume = 0; // the envelope's level, written to SOUNDxCNT
    uint8_t pan = 0;    // written to SOUNDxCNT
    int16_t pitch = 0;  // in 1/96 octave, from the base timer
    uint8_t velocity = 0;
    uint8_t state = 0;    // 0 free, 1 releasing, 2 release asked, 3 sounding, 4 start asked
    uint8_t quadrant = 0; // the vibrato's quarter
    uint8_t hardware = 0; // the sound unit's channel
    uint8_t frames = 0;   // the frames left of the note's ticks
    int8_t key = 0;
    uint16_t length = 0;  // the note's ticks left
    uint8_t envelope = 0; // the envelope's phase: 1 attack, 2 decay, 3 sustain, 5 release, 0 done
    uint8_t envelope_frames = 0;
    uint8_t modulation = 0; // the pitch modulation's phase: 1 sweep, 2 delay, 3 vibrato, 5 release, 0 done
    uint8_t modulation_frames = 0;
    uint8_t length_a = 0; // the lengths of the current phases, which the envelope and the modulation share
    uint8_t length_b = 0;
    uint16_t priority = 0;
    bool started = false; // the channel started in the last frame
};

// The driver's state between frames, for the tracks that a sound plays.
struct Track
{
    uint32_t instrument = 0; // the address of the instrument, or 0
    uint8_t volume = 0;
    int8_t bend = 0;
    uint8_t pan = 0;
    uint8_t program = 0;
    uint32_t flags = 0; // bits 0-1 the priority, 2-3 the hold, 4-7 a pitch offset on player 0
    uint8_t wait = 0;
    uint16_t tempo = 0;
    uint16_t carry = 0;
    uint16_t frames = 0;
    uint32_t position = 0; // the address of the next event, or 0 once the track has ended
    uint32_t loop = 0;
};

struct PlayerState
{
    uint16_t sound = 0;
    uint8_t delay = 0;
    uint8_t next_length = 0;
    uint8_t state = 0; // 0 stopped, 1 playing, 2 paused
    uint8_t volume = 0;
    int8_t pitch = 0;
    int8_t tempo = 0;
    uint8_t fade_length = 0;
    uint8_t fade_count = 0;
    uint8_t level_length = 0;
    uint8_t level_count = 0;
    uint8_t fade = 0;  // 1 a fade-out that stops the sound, 2 a fade-in at the start
    uint8_t level = 0; // 1 a fade-out that pauses, 2 a fade-in that resumes, 3 a level
    uint8_t target = 0;
    uint8_t saved = 0;
    uint8_t priority = 0;
    uint8_t max_tracks = 0;
    uint8_t set = 0;
    int8_t pan = 0;
    bool set_b = false;
    std::vector<Track> tracks;
};

// A track's 0xF8 (the loop's start) or 0xF9 (the jump back to it) in a frame.
struct LoopMark
{
    int player = 0;
    int track = 0;
    bool jump = false;
};

// Thrown when a sound reads set B and no block is loaded as set B. Some sounds of set A play the programs of whichever
// scene the game has loaded.
class NoSetB : public std::runtime_error
{
public:
    NoSetB() : std::runtime_error("the sound plays programs of set B, and no block is loaded as set B")
    {
    }
};

class Player
{
public:
    // A driver with `set_a` and, when it isn't null, `set_b`. The game loaded `set_b` from slot `set_b_slot`. `header`
    // is the sound data's header: its channel groups at 0xE and its reverb at 6. The blocks must outlive the player.
    Player(std::span<const uint8_t> header, const Block& set_a, const Block* set_b, int set_b_slot,
           const Layout& layout);

    // Queues a message for the next frame. The driver takes 64 at most in a frame.
    void Post(uint32_t message);

    // The value that 0xFC and 0xFD test on player 0 (the ARM9's command 10).
    void SetBranchFlag(int value);

    // The pitch effect on player 0 (the ARM9's command 5): 1 counts with each note, 2 wobbles by up to 12/96 octave,
    // 3 alternates by 24/96 octave each frame.
    void SetPitchMode(int mode);

    // Runs a frame, and returns the register writes in the driver's order.
    std::vector<RegisterWrite> Frame();

    // Whether a player plays or a channel sounds.
    bool Busy() const;

    // The loops' starts and jumps of the last frame, in the driver's order.
    const std::vector<LoopMark>& LoopMarks() const;

    const std::vector<Channel>& Channels() const;
    const std::array<PlayerState, kPlayers>& Players() const;
    int Reverb() const;

private:
    void Dispatch(uint32_t message);
    void Start(uint32_t message);
    void Stop(uint32_t message);
    void Pause(uint32_t message);
    void Resume(uint32_t message);
    void ForPlayers(uint32_t message, void (Player::*action)(uint32_t));
    void SetLevel(uint32_t message);
    void SetChannelGroups(uint32_t message);
    void SetReverb(uint32_t message);
    void RunAction(int player, uint32_t flags, bool end);

    void Fade(int player);
    void Step(int player);
    bool PlayEvents(int player, int track, int before);
    void StartNotes(int player, int track, int before, uint32_t& position, uint32_t& instrument);
    int Allocate(int kind, uint32_t priority);
    uint16_t WaitFrames(Track& track, const PlayerState& player);
    void NoteTicks(Channel& channel);
    uint32_t ScaledTempo(uint16_t tempo, const PlayerState& player) const;

    void Update(Channel& channel);
    uint8_t Envelope(Channel& channel, uint32_t instrument);
    int16_t Pitch(Channel& channel, uint32_t instrument, int key);
    void WriteChannel(const Channel& channel, uint32_t settings, bool start);
    void ClearChannel(const Channel& channel);
    void FreeChannels(int player);

    uint32_t SetAddress(bool b) const;

    // The address of the 8-byte entry of `sound` (from 1) in the table of sounds of set `set`: the offset of its header
    // from the set's block, then its flags.
    uint32_t SoundEntry(int set, int sound) const;

    uint8_t Read8(uint32_t address) const;
    uint16_t Read16(uint32_t address) const;
    uint32_t Read32(uint32_t address) const;
    void Write(uint32_t address, int size, uint32_t value);

    const Block& set_a_;
    const Block* set_b_;
    int set_b_slot_;
    Layout layout_;
    std::vector<uint32_t> messages_;
    std::array<PlayerState, kPlayers> players_;
    std::vector<Channel> channels_; // in the order of the driver's list
    std::size_t squares_end_ = 0;   // the ends of the groups in channels_
    std::size_t noise_end_ = 0;
    std::size_t active_end_ = 0; // the reverb takes the last two channels
    int branch_flag_ = 0;
    int pitch_mode_ = 0;
    uint16_t counter_ = 0;
    int16_t pitch_offset_ = 0;
    uint16_t cycle_ = 0;
    int reverb_ = 0;
    std::vector<RegisterWrite> writes_;
    std::vector<LoopMark> loop_marks_;
};

} // namespace ntdlib::spd
