// SPDX-License-Identifier: MIT

// A model of Ubisoft Milan's driver as it plays a piece of music: the sequencer, the mixer's voices that play the kit's
// samples, and the PSG's notes.
//
// The model has two modes. A conversion's mode leaves out what a MIDI file can't hold or what no real song uses, each
// with a warning. The strict mode follows the game's code on every path, for ubimilan::Output:
//
// - A kit's entry names whatever the resource table gives for it. The game checks nothing. The voice plays the bytes
//   that the address gives, in any of the cartridge's mirrors.
// - A note of the kit without a free voice takes voice -1. The game's records of a voice follow one another, each voice
//   with a second record at the fourth voice after it. The fourth record after voice -1 is voice 3, and the note plays
//   there.
// - The game resets in the sound update that finds a voice's volume above 64, and at a command or a next sequence that
//   it doesn't take. The model's song stops there (Reset()).
//
// The strict mode keeps no record of notes: a seek's snapshots copy the model.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/ubimilan/driver.h"

namespace ntdlib::ubimilan
{

using ntdlib::agb::RegisterWrite;

// The longest that a song plays: an hour. The model stops there, and so does a conversion.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// A note as the driver plays it.
struct Note
{
    int channel = 0;      // the sequence channel
    int key = 0;          // the sequence's key
    int velocity = 0;     // the sequence's velocity
    int program = 0;      // channel 9's offset into its kit, or a PSG channel's instrument
    uint32_t on = 0;      // the frame it starts on
    uint32_t off = 0;     // the frame its sound stops on, if `ended`
    bool ended = false;   // false while it still sounds
    uint32_t sample = 0;  // a kit note's sample resource
    uint32_t skip = 0;    // the sample's points before the note's first
    uint8_t duty = 0;     // a square note's NRx1, whose top 2 bits are the duty
    uint8_t envelope = 0; // the volume: a square's or the noise's NRx2, with its envelope, or the wave's NR32
    uint8_t control = 0;  // a noise note's NR43
    bool legato = false;  // a PSG note that goes on from the note before it, without a restart
    uint32_t command = 0; // the address of the command that started it
    uint32_t read = 0;    // the read of the sequence that ran the command, in Sequencer::SequenceReads()
};

// A pass of the track through a sequence of the piece.
struct SequenceRead
{
    uint32_t frame = 0;
    int place = 0;     // the sequence's place in the piece's list
    uint32_t base = 0; // the address of the sequence's first byte
    uint32_t last = 0; // the address of the last item that the pass read: a countdown or the end
};

// The cartridge's byte at bus address `address`: the three regions of the cartridge's wait states each show the whole
// cartridge. Other addresses read as 0 here.
uint8_t BusByte(const Rom& rom, uint32_t address);

// What one of the mixer's runs takes from a voice.
struct RunVoice
{
    uint32_t position = 0; // the voice's first point of the run
    uint32_t volume = 0;
    bool heard = false; // the mixer adds the run's points: the voice still plays after the run moved it on
};

using MixerRun = std::array<RunVoice, kVoiceCount>;

// A note of the kit that the strict mode played in a frame, or its note-off.
struct KitEvent
{
    bool on = false;
    uint32_t data = 0; // the sample's first point
    uint32_t length = 0;
    bool loop = false;
    uint32_t loop_start = 0; // addresses, like `data`
    uint32_t loop_end = 0;
    uint32_t volume = 0;
};

// The driver playing one piece of music, frame by frame.
class Sequencer
{
public:
    // Keeps references to `rom` and `info`. They have to outlive this. Starts song `song` as the game does, in the
    // strict mode when `strict` is true. With `kit` false, the game's switch of channel 9's kit is off: the kit's notes
    // and note-offs don't play.
    Sequencer(const Rom& rom, const DriverInfo& info, int song, bool strict = false, bool kit = true);

    // Returns false if the song can't be played: its list of sequences or its first sequence isn't in the ROM.
    bool Valid() const
    {
        return valid_;
    }

    // Runs a frame as the game does: the sequencer's step at VBlank, the game's sound update in its main loop, then the
    // mixer's runs until the next frame. Returns the frame's writes to the PSG's registers, and for the first frame,
    // the writes that the piece's start made.
    std::vector<RegisterWrite> Step();

    // Runs a frame in the same way, and adds what each of the mixer's runs took from the voices to `runs`.
    std::vector<RegisterWrite> Step(std::vector<MixerRun>& runs);

    // The stores that the game's start and the driver's init make, in order, up to the first piece of music: the sound
    // on, the FIFOs on their sides and on Timer 0, the output's resolution, the first wave in both banks of the wave
    // RAM and the PSG's volumes. Throws nothing: an image without a wave table gives the stores with zeros in place of
    // the wave.
    std::vector<RegisterWrite> InitWrites() const;

    // In the strict mode, the notes of the kit that the last frame played, and the note-offs. Whether the piece keeps a
    // voice for the kit is Piece().KeepsKitVoice().
    const std::vector<KitEvent>& KitEvents() const
    {
        return kit_events_;
    }

    // In the strict mode, whether the game has reset.
    bool Reset() const
    {
        return reset_;
    }

    // Returns true while the track plays.
    bool TrackActive() const
    {
        return active_;
    }

    // Returns true while the sequencer plays, or a voice does.
    bool Playing() const;

    // Returns the frames run so far.
    uint32_t Frame() const
    {
        return frame_;
    }

    // Returns the notes started so far, in the order they started.
    const std::vector<Note>& Notes() const
    {
        return notes_;
    }

    // Returns the piece of music that the sequencer read when it started the song.
    const MusicPiece& Piece() const
    {
        return piece_;
    }

    // Returns the frames on which each sequence of the piece started, in order, the first on frame 0.
    const std::vector<uint32_t>& SequenceStarts() const
    {
        return sequence_starts_;
    }

    // Returns each pass of the track through a sequence, in order. The strict mode keeps none.
    const std::vector<SequenceRead>& SequenceReads() const
    {
        return reads_;
    }

    // Returns the problems found in the song's data.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

    // The track as the driver keeps it: the countdown, the next command, the next sequence, whether it plays and
    // whether an end command has been read.
    std::array<uint32_t, 5> TrackState() const;

    // Returns false if the mixer doesn't play voice `v`.
    bool VoicePlays(int v) const
    {
        return !voices_[size_t(v)].stopped;
    }

    // Voice `v` as the mixer keeps it: its next point, its loop's start, its end, its loop's end, its volume and
    // whether it loops.
    std::array<uint32_t, 6> VoiceState(int v) const;

private:
    // A voice of the mixer, and the driver's note of whether a sound has it.
    struct Voice
    {
        bool busy = false;   // a sound has it
        bool stopped = true; // the mixer doesn't play it
        uint32_t position = 0;
        uint32_t end = 0;
        bool loop = false;
        uint32_t loop_start = 0;
        uint32_t loop_end = 0;
        uint32_t volume = 0;
        int note = -1; // the note it plays, or -1
    };

    // The modulation of an instrument, as the driver keeps it for a PSG channel: a triangle in cents after a delay.
    // The value counts the delay down, then contains the triangle in 16.16 fixed point.
    struct Modulation
    {
        uint32_t state = 0; // 0 off, 1 the delay, 2 and 3 rising, 4 falling
        int32_t value = 0;
        int32_t step = 0;  // the change each frame, in 16.16 fixed point
        int32_t depth = 0; // the cents at which the triangle turns
    };

    // An instrument's pitch mode, as the driver keeps it for a PSG channel: a glide in cents from one note's pitch to
    // the next.
    struct PitchMode
    {
        uint32_t state = 0; // 0 off, 1 a note's pitch, 2 gliding, 3 at the glide's end
        int32_t frames = 0; // the frames that a glide takes
        int32_t step = 0;   // the change each frame, in cents
        int32_t current = 0;
        int32_t target = 0;
        int32_t previous = 0; // the target before the last
    };

    // A PSG channel's state, as the driver keeps it. The driver's init clears it. A piece's start doesn't.
    struct PsgChannel
    {
        int program = 0;
        int index = 0;        // the place in the frequency table of its last note's key
        bool playing = false; // a note-on has started the channel, and no note-off has stopped it
        Modulation modulation;
        PitchMode pitch;
        int note = -1; // the note that sounds, or -1
    };

    void StartSamplePiece(int song);
    void NextSamplePass(Voice& voice);
    void StartVoice(Voice& voice, const GameSample& sample, int velocity);
    void GameKitNoteOn(int key, int velocity);
    void GameKitNoteOff();
    void GamePsgNoteOn(int channel, int key, int velocity);
    void GamePieceUpdate();
    void ResetGame(const std::string& why);
    std::vector<RegisterWrite> Step(std::vector<MixerRun>* runs);
    void SequencerStep();
    void SoundUpdate();
    MixerRun RunMixer();
    void RunCommand(int status);
    void NoteOn(int channel, int key, int velocity);
    void NoteOff(int channel, int key);
    void KitNoteOn(int key, int velocity);
    void KitNoteOff();
    void NoiseNoteOn(int key, int velocity);
    void NoiseNoteOff(int key);
    void PsgNoteOn(int channel, int key, int velocity);
    void PsgStart(int channel, int index, int velocity);
    void PsgLegato(int channel, int index);
    void Glide(PitchMode& pitch, uint32_t inst, int32_t target) const;
    void PsgNoteOff(int channel, int key);
    void PsgUpdate();
    uint32_t Instrument(int channel, int program) const;
    uint16_t Frequency(int index) const;
    uint16_t FineFrequency(int32_t cents) const;
    void Program(int channel, int program);
    Note NewNote(int channel, int key, int velocity, int program) const;
    void EndNote(int index, uint32_t frame);
    void Stop(const std::string& why);
    void Warn(const std::string& message);
    uint32_t NextSequence(int ahead) const;
    void Write(uint32_t address, uint8_t size, uint32_t value);

    const Rom& rom_;
    const DriverInfo& info_;
    bool strict_ = false;
    bool valid_ = false;
    bool reset_ = false;
    MusicPiece piece_;
    uint32_t frame_ = 0;
    uint64_t runs_ = 0; // the mixer's runs so far

    // The track.
    uint32_t wait_ = 0;
    uint32_t pointer_ = 0;

    // The next sequence's countdown and commands, or 0. From an end command to the sound update, it holds the address
    // of the resource that the game's callback gave the track.
    uint32_t next_ = 0;

    bool active_ = false;

    // The game's flag that the piece plays. The game's callback clears it once the track stops.
    bool piece_playing_ = false;

    bool ending_ = false; // an end command was read

    // The strict mode: the sequence after the next at the piece's start. The game's callback gives the track that
    // sequence at each end command.
    uint32_t after_next_ = 0;

    // A sequence ended. The game's sound update notes it.
    bool changed_ = false;

    int index_ = 0; // the sequence of the piece that plays

    // The kit, its voices and the PSG.
    bool kit_on_ = true; // the game's switch of the kit, the word at +0x138 of the track
    int kit_program_ = 0;
    bool kit_voice_fixed_ = false;
    int kit_voice_ = 0;
    std::array<Voice, kVoiceCount> voices_ = {};
    std::array<PsgChannel, kPsgChannels> psg_ = {};

    std::vector<Note> notes_;
    std::vector<uint32_t> sequence_starts_;
    std::vector<SequenceRead> reads_;
    uint32_t command_ = 0; // the address of the command that runs
    std::vector<RegisterWrite> writes_;
    std::vector<KitEvent> kit_events_;
    std::vector<std::string> warnings_;
};

} // namespace ntdlib::ubimilan
