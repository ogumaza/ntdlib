// SPDX-License-Identifier: MIT

// A model of the player of standard MIDI files in Hey You, Pikachu!, by Ambrella, on the compact form of the synthesis
// driver. The driver calls the player once for each frame. Each call moves the player on by a fixed time, whatever the
// frame's length.
//
// Several players share one pool of voices. A player reads its file, keeps a list of the notes that are on, and turns
// them off when it stops or loops. CC 102 marks a loop's start and CC 103 its end. From the sixth pass on, two
// passes of every three leave out a random set of the channels from 8 up. A voice's level and pitch update on every
// call. A decay or a release runs as one long ramp. The voice ends once its envelope falls under a limit.
//
// The player's state is plain values. A file stays where the caller put it. A copy of the object is a snapshot.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "ntdlib/n64/al_player.h"
#include "ntdlib/n64/al_synth.h"
#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/bank_file.h"

namespace ntdlib::n64
{

struct AmbrellaPlayerConfig
{
    int players = 4;
    int voices = 40;
    bool mono = false;

    // The time that each call of the player takes in the game's reckoning.
    double update_seconds = 0.00575;
};

// A note that starts. `offset` is the place in the file of the event's first byte.
struct AmbrellaNoteStart
{
    int player = 0;
    int channel = 0;
    uint8_t key = 0;
    uint8_t velocity = 0;
    uint32_t offset = 0;
};

class AmbrellaPlayer
{
public:
    static constexpr int kChannels = 16;
    static constexpr int kFullMasterVolume = 128;

    AmbrellaPlayer(const AmbrellaPlayerConfig& config, AlSynth& synth);

    // The bank that every player plays. The file must outlive the player.
    void SetBank(const AlSoundBank& bank);

    // The level of all the players. `kFullMasterVolume` is full.
    void SetMasterVolume(uint8_t volume);

    // Starts a standard MIDI file of format 0 on the first idle player, at full level. Returns the player, or -1 if no
    // player is idle or the file isn't one that the game's reader takes. The file must stay where it is.
    int StartSong(std::span<const uint8_t> file);

    bool Playing(int player) const;

    // Stops a player after `seconds`. Its notes then release.
    void StopSong(int player, double seconds);

    // Fades a player to `level`, from 0 to 1, over `seconds`.
    void FadeTo(int player, double seconds, double level);

    // A factor on the speed of a player's song.
    void SetSpeed(int player, double speed);

    // The driver's call.
    void Handle(AlSynth& synth);

    bool AnyPlaying() const;

    // A voice's track, for the marks in a list: a player's channels take 16 tracks each.
    static constexpr int TrackOf(int player, int channel)
    {
        return (player * kChannels) + channel;
    }

    // Gives the value that the game reads from its CPU's count register, for the set of channels of a pass.
    std::function<uint32_t()> random;

    // Called for each note that starts.
    std::function<void(const AmbrellaNoteStart&)> on_note;

private:
    // The tools that check the model against a game run read and set the state below through ModelAccess.
    friend struct ModelAccess;

    static constexpr int kHeldNotes = 32;
    static constexpr int kEventBytes = 20;

    struct Channel
    {
        int32_t instrument = kNoObject;
        uint16_t bend = 8192;
        uint16_t bend_range = 256; // semitones in 1/128 of a semitone
        uint16_t pan = 8192;
        uint8_t volume = 100;
        uint8_t fx_mix = 0;
        uint8_t sustain = 0;
        uint16_t expression = 16256;
        uint8_t parameter_low = 127;
        uint8_t parameter_high = 127;
        uint8_t last_key = 64;
        uint8_t portamento = 0;
        uint8_t portamento_time = 0;
        uint8_t vibrato_depth = 0;
        uint8_t tremolo_depth = 0;
    };

    struct Voice
    {
        int next = -1;
        int previous = -1;
        bool on = false;
        bool sustained = false;
        int32_t instrument = kNoObject;
        int32_t sound = kNoObject;
        int synth_voice = -1;
        int channel = 0;
        uint8_t key = 0;
        uint8_t velocity = 0;
        uint16_t level = 0;
        uint8_t pan = 0;
        uint16_t vibrato_phase = 0;
        uint16_t tremolo_phase = 0;
        int16_t sweep = 0;
        float pitch = 0.0F;
        int32_t envelope = 0;
        int32_t envelope_rate = 0;
    };

    struct Player
    {
        int number = 0;

        // The file's reader.
        std::span<const uint8_t> file;
        std::size_t position = 0;
        std::size_t end = 0;
        uint8_t running_status = 0;
        int32_t division = 0;
        int32_t tempo = 500000;
        double speed = 1.0;
        bool stopped = true;
        bool flush_notes = false;
        bool flush_pedals = false;
        bool reset_pending = false;
        double stop_timer = -1.0;
        double fade_time = -1.0;
        double fade_target = 0.0;
        double level_now = -1.0;
        bool has_loop = false;
        std::size_t loop_position = 0;
        uint8_t loop_status = 0;
        uint16_t channel_mask = 0xFFFF;
        uint16_t passes = 0;
        int held_notes = 0;
        std::array<uint8_t, kHeldNotes> held_channels{};
        std::array<uint8_t, kHeldNotes> held_keys{};
        std::array<uint8_t, kChannels> pedals{};

        // The event that comes next, and the time to it.
        bool has_event = false;
        std::array<uint8_t, kEventBytes> event{};
        uint32_t event_offset = 0;
        double delta = -1.0;

        // What the events set.
        int head = -1;
        uint16_t master = 16384;
        double fade_remaining = 0.0;
        double fade_rate = 0.0;
        double fade_level = 1.0;
        std::array<Channel, kChannels> channels{};
    };

    // The reader. Each returns whether `event` holds an event.
    bool ReadFromFile(Player& player, double& delta);
    bool FlushHeld(Player& player);
    uint32_t ReadFileEvent(Player& player);
    bool PassEvent(Player& player);
    bool PassController(Player& player, uint8_t channel);
    bool ReadEvent(Player& player);
    uint8_t NextByte(Player& player);
    uint32_t NextLength(Player& player);
    void StopReader(Player& player);

    // The events.
    void RunEvents(AlSynth& synth, Player& player);
    void Dispatch(AlSynth& synth, Player& player);
    void NoteOn(AlSynth& synth, Player& player, int channel, uint8_t key, uint8_t velocity);
    void NoteOff(AlSynth& synth, Player& player, int channel, uint8_t key);
    void Controller(AlSynth& synth, Player& player, int channel, uint8_t controller, uint8_t value);
    void SetSustain(AlSynth& synth, Player& player, int channel, uint8_t value);
    void ResetControllers(AlSynth& synth, Player& player, int channel);
    void ResetPlayer(AlSynth& synth, Player& player);
    void SystemMessage(Player& player);

    // The voices.
    int32_t FindSound(int32_t instrument, uint8_t key, uint8_t velocity) const;
    int TakeVoice(Player& player);
    void EndVoice(AlSynth& synth, Player& player, int voice);
    void ReleaseVoice(AlSynth& synth, Player& player, int voice);
    uint16_t VoiceLevel(const Player& player, const Voice& voice) const;
    uint8_t VoicePan(const Player& player, const Voice& voice) const;
    float VoicePitch(const Player& player, const Voice& voice) const;
    void UpdatePan(AlSynth& synth, const Player& player, Voice& voice);
    void UpdateVoices(AlSynth& synth, Player& player);

    static void RunFade(Player& player, double seconds);

    AmbrellaPlayerConfig config_;
    AlSoundBank bank_;
    uint8_t master_volume_ = kFullMasterVolume;
    int free_head_ = -1;
    std::vector<Voice> voices_;
    std::vector<Player> players_;
};

// Appends one frame's commands to `list`. The game's frames are one chunk long, and the driver calls the player once
// for each. A longer frame takes one call for each chunk.
void BuildAmbrellaFrame(AlSynth& synth, AmbrellaPlayer& player, AudioList& list, AudioMemory& memory, uint32_t output,
                        int samples);

} // namespace ntdlib::n64
