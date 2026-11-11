// SPDX-License-Identifier: MIT

#include "ntdlib/rd2/sequencer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/rd2/driver.h"

#include "sequencer_internal.h"

namespace ntdlib::rd2
{

// An instrument gives a note a region to play: the instrument's, a drum kit's for the key, a key split's, or the
// driver's region in RAM with the instrument's sample for the key.
Lookup LookUpInstrument(const Rom& rom, const DriverInfo& info, uint16_t bank, uint16_t instrument, uint8_t note)
{
    Lookup lk;
    const uint32_t base = BankAddress(rom, info, bank);
    const uint32_t inst = base + rom.U16(base + 2 * uint32_t(instrument));
    const uint8_t kind = rom.U8(inst);
    if ((kind & 0xF0) == 0)
    {
        lk.region = inst;
        lk.envelope = base + rom.U16(inst + 4);
        lk.found = true;
    }
    else if (kind == 0x10)
    {
        const uint32_t entry = base + rom.U16(inst + 2) + 4 * uint32_t(uint8_t(note - rom.U8(inst + 4)));
        lk.drum = true;
        lk.drum_pan = rom.U8(entry + 2);
        lk.region = base + rom.U16(entry);
        lk.envelope = base + rom.U16(lk.region + 4);
        lk.found = true;
    }
    else if (kind == 0x11)
    {
        lk.key_sample = true;
        lk.key_sample_index = rom.U16(base + rom.U16(inst + 2) + 2 * uint32_t(note));
        lk.region = 0;
        lk.envelope = info.key_envelope;
        lk.found = true;
    }
    else if (kind == 0x12)
    {
        uint32_t entry = base + rom.U16(inst + 2);
        for (int i = 0; i < 256 && note > rom.U8(entry); i++)
        {
            entry += 4;
        }
        lk.region = base + rom.U16(entry + 2);
        lk.envelope = base + rom.U16(lk.region + 4);
        lk.found = true;
    }

    lk.missing = lk.region == base;
    if (!lk.found || !lk.region)
    {
        return lk;
    }

    if (RegionType(rom, lk.region) == kWaveType)
    {
        lk.wave = base + rom.U16(inst + 2);
    }
    if (HasPsgTable(rom, lk.region))
    {
        lk.psg_table = base + rom.U16(lk.region + 2);
    }

    return lk;
}

uint32_t BankAddress(const Rom& rom, const DriverInfo& info, uint16_t bank)
{
    return info.banks + rom.U32(info.banks + 4 * uint32_t(bank));
}

uint32_t SampleTable(const Rom& rom, const DriverInfo& info, uint16_t bank)
{
    const uint16_t set = rom.U16(info.bank_sample_sets + 2 * uint32_t(bank));
    return info.sample_sets + rom.U32(info.sample_sets + 4 * uint32_t(set));
}

uint32_t SampleAddress(const Rom& rom, const DriverInfo& info, uint16_t bank, uint32_t index)
{
    const uint32_t samples = SampleTable(rom, info, bank);
    return samples + rom.U32(samples + 4 * index);
}

int PitchIndex(uint8_t note, uint8_t tuning)
{
    return std::clamp(int(int16_t(uint16_t(note + kUnityIndex - tuning))), 0, kLastIndex + 1);
}

uint8_t RegionType(const Rom& rom, uint32_t region)
{
    return region ? rom.U8(region) : kSampleType;
}

uint8_t RegionFlags(const Rom& rom, uint32_t region)
{
    return region ? rom.U8(region + 1) : 0;
}

uint8_t RegionRelease(const Rom& rom, uint32_t region)
{
    return region ? rom.U8(region + 6) : 0;
}

uint8_t RegionTuning(const Rom& rom, uint32_t region)
{
    return region ? rom.U8(region + 7) : kKeySampleTuning;
}

uint8_t RegionSweep(const Rom& rom, uint32_t region)
{
    return region ? rom.U8(region + 8) : 0;
}

bool HasPsgTable(const Rom& rom, uint32_t region)
{
    return (RegionFlags(rom, region) & 1) != 0;
}

uint8_t PsgTableEntry(const Rom& rom, uint32_t table, uint32_t frame)
{
    return frame < rom.U16(table) ? rom.U8(table + 2 + frame) : LastPsgTableEntry(rom, table);
}

uint8_t LastPsgTableEntry(const Rom& rom, uint32_t table)
{
    return rom.U8(table + 1 + rom.U16(table));
}

uint8_t NoiseTableEntry(const Rom& rom, const DriverInfo& info, uint32_t index)
{
    return rom.U8(info.noise_table + std::min<uint32_t>(index, kLastIndex));
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int song, int version)
    : rom_(rom),
      info_(info),
      traits_(TraitsOf(info.revision)),
      sequence_(song),
      version_(version),
      effect_(SongEffect(info, song))
{
    valid_ = effect_ ? effect_->address != 0 : song >= 0 && song < int(info.sequence_addresses.size());

    // The voices as the driver's init leaves them: the sample voices in the free list, and a PSG voice for each of the
    // PSG's channels.
    for (int v = 0; v < kSampleVoices; v++)
    {
        voices_[size_t(v)].prev = v - 1;
        voices_[size_t(v)].next = v + 1 < kSampleVoices ? v + 1 : -1;
    }
    for (int i = 0; i < kPsgVoices; i++)
    {
        voices_[size_t(kSampleVoices + i)].type = uint8_t(kSquare1Type + i);
    }
    free_ = 0;

    // The registers that init writes and the PSG voices read back.
    registers_[kNr10 - kRegisters] = 0x08;
    registers_[kNr12 - kRegisters] = 0xF0;
    registers_[kNr51 - kRegisters] = 0xFF;
}

void Sequencer::Warn(const std::string& text)
{
    WarnOnce(warnings_, text);
}

std::vector<int> Sequencer::ActiveOrder() const
{
    std::vector<int> order;
    for (int v = active_; v >= 0 && order.size() < voices_.size(); v = voices_[size_t(v)].next)
    {
        order.push_back(v);
    }

    return order;
}

void Sequencer::Write(uint32_t address, uint8_t size, uint32_t value)
{
    value &= size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    if (unit_ != 0)
    {
        // A grown unit's registers lie apart from the console's, and a store there restarts none of the console's
        // channels.
        const uint32_t base = agb::kGrownPsgRegisters + uint32_t(unit_ - 1) * agb::kGrownPsgBytes;
        writes_.push_back({base + (address - kRegisters), size, value});
        std::array<uint8_t, 0x40>& registers = grown_registers_[size_t(unit_ - 1)];
        for (uint8_t i = 0; i < size; i++)
        {
            registers[address + i - kRegisters] = uint8_t(value >> (8 * i));
        }

        return;
    }

    writes_.push_back({address, size, value});
    for (uint8_t i = 0; i < size; i++)
    {
        const uint32_t at = address + i - kRegisters;
        const uint8_t byte = uint8_t(value >> (8 * i));
        if (at < registers_.size())
        {
            registers_[at] = byte;
        }
        for (int c = 0; c < kPsgVoices; c++)
        {
            if (address + i == kRestartRegisters[c] && (byte & kRestartBit))
            {
                restarted_ = uint8_t(restarted_ | (1 << c));
            }
        }
    }
}

uint32_t Sequencer::ReadBack(uint32_t address, uint8_t size) const
{
    const std::array<uint8_t, 0x40>& registers = unit_ != 0 ? grown_registers_[size_t(unit_ - 1)] : registers_;
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; i++)
    {
        const uint32_t at = address + i - kRegisters;
        value |= uint32_t(at < registers.size() ? registers[at] : 0) << (8 * i);
    }

    return value;
}

uint32_t& Sequencer::CurrentWave()
{
    return unit_ != 0 ? grown_waves_[size_t(unit_ - 1)] : current_wave_;
}

// Carries out the play request: stops what the player was playing and starts each of the sequence's tracks.
void Sequencer::Start()
{
    started_ = true;
    if (!valid_)
    {
        return;
    }
    if (effect_)
    {
        StartEffect();
        return;
    }

    // The game's play-music routine requests the sample voices that the sequence plays with, then the sequence, then
    // the player's second volume or the mute of some of its tracks. The driver handles the requests in that order.
    const MusicSettings settings = VersionSettings(info_, sequence_, version_);
    SetSampleVoices(settings.sample_voices);

    StopPlayer();
    player_ = Player();
    player_.volume2 = uint8_t(settings.volume2);
    player_.sequence = info_.sequence_addresses[size_t(sequence_)];
    player_.banks = info_.bank_list_addresses[size_t(sequence_)];

    int count = rom_.S8(player_.sequence);
    if (count > kPlayerTracks)
    {
        Warn("the sequence has " + std::to_string(count) + " tracks, more than a player can have");
        count = kPlayerTracks;
    }
    for (int i = 0; i < count; i++)
    {
        const uint32_t offset = rom_.U16(player_.sequence + 2 + 2 * uint32_t(i));
        if (offset == 0)
        {
            continue;
        }

        const int slot = AllocateTrack();
        player_.tracks[size_t(i)] = slot;
        if (slot >= 0)
        {
            StartTrack(slot, player_.sequence + offset, i, 0);
        }
    }

    // The request that mutes tracks sets the mute of each track in its mask that the player has.
    for (int i = 0; i < kPlayerTracks; i++)
    {
        const int slot = player_.tracks[size_t(i)];
        if (((settings.muted_tracks >> i) & 1) && slot >= 0)
        {
            tracks_[size_t(slot)].mute = 1;
        }
    }

    player_.active = true;
}

// Carries out the game's request for a sound effect: stops what the player was playing and starts the effect's track as
// the player's track 0. The request leaves the sample voices as they are.
void Sequencer::StartEffect()
{
    StopPlayer();
    player_ = Player();
    player_.sequence = info_.effect_sequence_addresses[size_t(effect_->sequence)];
    player_.banks = info_.effect_bank_list_addresses[size_t(effect_->sequence)];
    player_.effect = true;
    const int slot = AllocateTrack();
    player_.tracks[0] = slot;
    if (slot >= 0)
    {
        StartTrack(slot, effect_->address, 0, 0);
    }

    player_.active = true;
}

int Sequencer::AllocateTrack() const
{
    for (int slot = 0; slot < kTrackSlots; slot++)
    {
        if (!tracks_[size_t(slot)].active)
        {
            return slot;
        }
    }

    return -1;
}

// Starts a track in `slot` at `position`, after stopping the track the slot had. It starts with the player's first
// bank, centred, at full volume, with the notes' default length and velocity. A sound effect's track starts at a
// priority of 12 with an echo send of 7F. Like a track after C8, its notes wait for their length.
void Sequencer::StartTrack(int slot, uint32_t position, int number, uint64_t units)
{
    if (tracks_[size_t(slot)].active)
    {
        FreeTrack(slot);
    }

    Track& t = tracks_[size_t(slot)];
    t = Track();
    t.position = position;
    t.active = true;
    t.number = number;
    t.units = units;
    if (player_.effect)
    {
        t.priority = kEffectPriority;
        t.echo = kEffectEcho;
        t.timed_notes = 1;
    }
    SetBank(t, 0);
}

void Sequencer::FreeTrack(int slot)
{
    ReleaseTrackVoices(slot);
    tracks_[size_t(slot)].active = false;
}

void Sequencer::ReleaseTrackVoices(int slot)
{
    Track& t = tracks_[size_t(slot)];
    const uint8_t legato = t.legato;
    t.legato = 0;
    for (int v = t.voices; v >= 0;)
    {
        const int next = voices_[size_t(v)].track_next;
        Release(v);
        v = next;
    }
    t.legato = legato;
}

void Sequencer::StopPlayer()
{
    if (!player_.active)
    {
        return;
    }

    for (int& slot : player_.tracks)
    {
        if (slot >= 0)
        {
            FreeTrack(slot);
        }
        slot = -1;
    }
    player_.active = false;
}

// Sets a track's bank to entry `bank` of the player's list of banks, with the bank's first instrument and the sample
// set it plays from, as C7 and a track's start do.
void Sequencer::SetBank(Track& t, uint16_t bank)
{
    t.bank = bank;
    t.instrument = 0;
    t.samples = SampleTable(rom_, info_, BankOf(t));
}

// Returns the bank in the track's entry of the player's list of banks.
uint16_t Sequencer::BankOf(const Track& t) const
{
    return rom_.U16(player_.banks + 2 * uint32_t(t.bank));
}

uint32_t Sequencer::ReadVarLength(Track& t)
{
    uint32_t value = rom_.U8(t.position++);
    if (value & 0x80)
    {
        value = ((value & 0x7F) << 8) | rom_.U8(t.position++);
    }

    return value;
}

void Sequencer::Step()
{
    events_.clear();
    reads_.clear();
    writes_.clear();
    restarted_ = 0;
    if (!started_)
    {
        Start();
    }

    // Each of the player's tracks reads its commands until its count of time is in the future, then takes the frame's
    // tempo off the count. A player stops when none of its tracks is left.
    phase_ = Event::kCommands;
    if (player_.active)
    {
        bool running = false;
        for (size_t i = 0; i < player_.tracks.size(); i++)
        {
            const int slot = player_.tracks[i];
            if (slot < 0)
            {
                continue;
            }

            if (RunTrack(slot))
            {
                running = true;
            }
            else
            {
                player_.tracks[i] = -1;
            }
        }
        if (!running)
        {
            StopPlayer();
        }
    }

    phase_ = Event::kPsgVoices;
    UpdatePsgVoices();
    phase_ = Event::kMixer;
    UpdateSampleVoices();
    elapsed_ += uint64_t(FrameTempo());
}

// Runs a track's commands for a frame. Returns false if it isn't active or has ended.
bool Sequencer::RunTrack(int slot)
{
    Track& t = tracks_[size_t(slot)];
    if (!t.active)
    {
        return false;
    }

    if (t.units == kFresh)
    {
        t.units = elapsed_;
    }

    for (int n = 0; t.counter <= 0; n++)
    {
        if (n == kMaxCommands)
        {
            Warn("track " + std::to_string(t.number) + " runs without waiting at " + Hex(t.position) +
                 ", which would hang the driver");
            FreeTrack(slot);
            return false;
        }

        const uint32_t at = t.position;
        if (keep_times_)
        {
            t.seen.emplace(at, t.units);
        }

        command_units_ = t.units;
        command_at_ = at;
        const uint8_t op = rom_.U8(t.position++);
        if (keep_reads_)
        {
            reads_.push_back({t.number, at, t.units, op, uint8_t(t.depth), t.timed_notes == 1});
        }
        if (op <= 0xBF)
        {
            PlayNote(slot, op);
            continue;
        }
        if (op == 0xC0 || op == 0xC1)
        {
            if (op == 0xC1)
            {
                t.wait = uint16_t(ReadVarLength(t));
            }

            const uint32_t wait = uint32_t(t.wait) * kTickUnits;
            t.counter += int32_t(wait);
            t.units += wait;
            continue;
        }
        if ((op & 0xF0) == 0xD0)
        {
            // A slide for the next note, from or to another note.
            t.slide_flags = op & 0x0F;
            t.slide_note = uint8_t(rom_.U8(t.position) + t.transpose);
            t.slide_length = rom_.U8(t.position + 1);
            t.position += 2;
            t.slide_delay = 0;
            if (op & 1)
            {
                t.slide_delay = rom_.U8(t.position++);
            }
            t.slide = 1;
            continue;
        }

        if (!RunCommand(slot, op, at))
        {
            return false;
        }
    }

    t.counter -= int32_t(player_.tempo);
    t.counter -= int32_t(player_.tempo_adjust);

    return true;
}

bool Sequencer::RunCommand(int slot, uint8_t op, uint32_t at)
{
    Track& t = tracks_[size_t(slot)];
    switch (op)
    {
    case 0xC2:
        t.instrument = rom_.U8(t.position++);
        break;

    case 0xC3:
        t.pan = rom_.U8(t.position++);
        events_.push_back(MakeEvent(Event::kPan, t.number, -1, 0, t.units, t.pan));
        break;

    case 0xC4:
        t.priority = rom_.U8(t.position++);
        break;

    case 0xC5:
    case 0xC6:
        ReleaseTrackVoices(slot);
        t.legato = op == 0xC5 ? 1 : 0;
        break;

    case 0xC7:
        SetBank(t, rom_.U8(t.position++));
        break;

    case 0xC8:
        t.timed_notes = 1;
        break;

    case 0xC9:
        t.timed_notes = 0;
        break;

    case 0xCA:
        // A call to the game's code. The game may not have given the driver any.
        Warn("the sequence calls the game's code with CA, which the model skips");
        t.position++;
        break;

    case 0xE0:
        t.volume = rom_.U8(t.position++);
        events_.push_back(MakeEvent(Event::kTrackVolume, t.number, -1, 0, t.units, t.volume));
        break;

    case 0xE1:
        t.bend = rom_.S8(t.position++);
        break;

    case 0xE2:
        t.bend_range = rom_.U8(t.position++);
        break;

    case 0xE3:
        t.echo = rom_.U8(t.position++);
        break;

    case 0xE4:
        // The Super Mario Advance 2 revision's tempo is a byte.
        player_.tempo = traits_.byte_tempo ? rom_.U8(t.position++) : uint16_t(ReadVarLength(t));
        break;

    case 0xE5:
        t.lfo_delay = rom_.U8(t.position++);
        break;

    case 0xE6:
        t.lfo_rate = rom_.U8(t.position++);
        break;

    case 0xE7:
        t.lfo_depth = rom_.U8(t.position++);
        break;

    case 0xE8:
        t.slide = 0;
        break;

    case 0xE9:
        t.transpose = rom_.U8(t.position++);
        break;

    case 0xEA:
        // The Super Mario Advance 2 revision has no player volume, and EA is one of the commands that do nothing.
        if (traits_.player_volume)
        {
            player_.volume = rom_.U8(t.position++);
            events_.push_back(MakeEvent(Event::kPlayerVolume, t.number, -1, 0, t.units, player_.volume));
        }
        break;

    case 0xF0:
        {
            t.position = player_.sequence + rom_.U16(t.position);
            Event e = MakeEvent(Event::kJump, t.number, -1, 0, t.units, t.position);
            const auto seen = t.seen.find(t.position);
            e.target_units = seen != t.seen.end() ? seen->second : kUnreadTarget;
            events_.push_back(e);
            break;
        }

    case 0xF4:
        {
            const uint32_t to = player_.sequence + rom_.U16(t.position);
            if (t.depth == kMaxDepth)
            {
                Warn("F4 at " + Hex(at) + " nests deeper than the driver has room for");
                FreeTrack(slot);
                return false;
            }

            t.stack[size_t(t.depth++)] = t.position + 2;
            t.position = to;
            break;
        }

    case 0xF8:
        StartTrackFrom(slot, at);
        break;

    case 0xFF:
        if (t.depth == 0)
        {
            events_.push_back(MakeEvent(Event::kTrackEnd, t.number, -1, 0, t.units, 0));
            FreeTrack(slot);
            return false;
        }

        t.position = t.stack[size_t(--t.depth)];
        break;

    default:
        break;
    }

    return true;
}

// Plays the note in `op`, from 00 to BF, with the track's length and velocity. A note from 60 to BF comes with a new
// length and velocity. The track keeps them for the notes after it. After C8, the track waits for the note's length.
void Sequencer::PlayNote(int slot, uint8_t op)
{
    Track& t = tracks_[size_t(slot)];
    uint8_t note = op;
    if (op >= 0x60)
    {
        t.length = uint16_t(ReadVarLength(t));
        t.velocity = rom_.U8(t.position++);
        note = uint8_t(op - 0x60);
    }

    const uint32_t length = uint32_t(t.length) * kTickUnits;
    NoteOn(slot, note, t.velocity, length);
    if (t.timed_notes == 1)
    {
        t.counter += int32_t(length);
        t.units += length;
    }
}

// Runs F8: starts one of the player's tracks with the settings of the track in `slot`. The warnings give `at` as F8's
// address.
void Sequencer::StartTrackFrom(int slot, uint32_t at)
{
    Track& t = tracks_[size_t(slot)];
    const int number = rom_.U8(t.position);
    const uint32_t to = player_.sequence + rom_.U16(t.position + 1);
    t.position += 3;
    if (number >= kPlayerTracks)
    {
        Warn("F8 at " + Hex(at) + " starts track " + std::to_string(number) + ", past the player's 10");
        return;
    }

    int other = player_.tracks[size_t(number)];
    if (other < 0)
    {
        other = AllocateTrack();
        player_.tracks[size_t(number)] = other;
        if (other < 0)
        {
            Warn("F8 at " + Hex(at) + " finds no free track");
            return;
        }
    }

    // The new track's time starts with the first frame it runs in: this one if it comes later in the player's order, or
    // is this one. The copy comes after StartTrack(). An F8 that starts the track running it therefore keeps the
    // settings that StartTrack() gives it.
    StartTrack(other, to, number, other == slot ? elapsed_ : kFresh);
    Track& o = tracks_[size_t(other)];
    o.samples = t.samples;
    o.bank = t.bank;
    o.instrument = t.instrument;
    o.pan = t.pan;
    o.echo = t.echo;
    o.volume = t.volume;
    o.volume2 = t.volume2;
    o.priority = t.priority;
    o.bend = t.bend;
    o.bend_range = t.bend_range;
    o.transpose = t.transpose;
    events_.push_back(MakeEvent(Event::kTrackStart, t.number, -1, 0, t.units, uint32_t(number)));
}

void Sequencer::NoteOn(int slot, uint8_t note, uint8_t velocity, uint32_t length)
{
    Track& t = tracks_[size_t(slot)];
    const uint8_t given = note;
    if (t.mute)
    {
        return;
    }

    note = uint8_t(note + t.transpose);
    Event e;
    e.kind = Event::kNote;
    e.track = t.number;
    e.units = t.units;
    e.key = note;
    e.velocity = velocity;
    e.bank = BankOf(t);
    e.instrument = t.instrument;
    e.length = length;
    e.command = command_at_;
    e.lookup = LookUpInstrument(rom_, info_, e.bank, t.instrument, note);
    const Lookup& lk = e.lookup;
    if (!lk.found)
    {
        Warn("instrument " + std::to_string(t.instrument) + " of bank " + std::to_string(e.bank) +
             " isn't one the driver can play, so its notes are left out");
        events_.push_back(e);
        return;
    }

    if (lk.key_sample)
    {
        key_sample_ = lk.key_sample_index;
    }

    const uint32_t region = lk.region;
    const uint32_t frames = NoteFrames(region, length);
    const int v = TakeVoice(slot, given, lk);
    if (v < 0)
    {
        events_.push_back(e);
        return;
    }

    Voice& x = voices_[size_t(v)];
    x.id = next_id_++;
    e.voice = v;
    e.id = x.id;

    // Drums and instruments with a sample for each key play at their region's pitch.
    x.own_pan = lk.drum ? 1 : 0;
    if (lk.drum)
    {
        note = kFixedNote;
        x.pan = lk.drum_pan;
    }
    else if (lk.key_sample)
    {
        note = kFixedNote;
    }

    x.velocity = velocity;
    x.volume = 0;
    x.frames = uint16_t(frames);
    x.echo = t.echo;
    const uint8_t tuning = RegionTuning(rom_, region);
    x.base_pitch = PitchStep(x.type, note, tuning);
    x.note_pitch = x.base_pitch;
    StartSlide(t, x, note, tuning, frames);
    SetVoiceSound(t, x, lk);

    e.type = x.type;
    e.sample = x.sample;
    e.psg = x.psg;
    events_.push_back(e);

    if (frames == 0)
    {
        Release(v);
    }
}

// Returns the frames that a note of `length` units lasts. A note lasts its length at the tempo it starts at: with the
// game's change to the tempo unless the region opts out of it. The Super Mario Advance 2 revision keeps the length in
// units, and the frames, in 16 bits.
uint32_t Sequencer::NoteFrames(uint32_t region, uint32_t length) const
{
    const int tempo =
        (RegionFlags(rom_, region) & kNoTempoAdjustFlag) ? player_.tempo : player_.tempo + player_.tempo_adjust;
    return traits_.short_notes ? uint16_t(UnsignedDiv(uint16_t(length), uint32_t(tempo)))
                               : UnsignedDiv(length, uint32_t(tempo));
}

// Returns the voice that plays a note of the track in slot `slot`, or -1 if none can be had. `given` is the note before
// the transpose. In legato, the track's voice plays the note. Otherwise the note takes a voice, and the voice's
// envelope starts over.
int Sequencer::TakeVoice(int slot, uint8_t given, const Lookup& lk)
{
    const Track& t = tracks_[size_t(slot)];
    if (t.legato && t.voices >= 0)
    {
        return t.voices;
    }

    const int v = AllocateVoice(rom_.U8(info_.voice_classes + RegionType(rom_, lk.region)), t.priority);
    if (v < 0)
    {
        return -1;
    }

    LinkToTrack(slot, v);
    Voice& x = voices_[size_t(v)];
    x.note = traits_.keeps_note ? given : 0;
    x.position = 0;
    x.lfo_phase = 0;
    x.lfo_delay = t.lfo_delay;
    x.envelope = lk.envelope;
    x.level = 0;
    x.target = 0;
    x.segment = 0;
    x.point = 0xFF;
    x.release = RegionRelease(rom_, lk.region);
    x.region = lk.region;

    return v;
}

// Sets the voice's slide from the track's slide settings. A slide moves the pitch between the note and the slide's note
// over a share of the note's frames. `note` is the note that the voice plays, and `tuning` is the region's tuning.
void Sequencer::StartSlide(Track& t, Voice& x, uint8_t note, uint8_t tuning, uint32_t frames)
{
    if (!t.slide)
    {
        x.slide_delay = 0;
        x.slide_frames = 0;
        x.slide = 0;
        x.slide_end = 0;
        x.slide_step = 0;
        return;
    }

    const uint32_t target = PitchStep(x.type, t.slide_note, tuning);
    x.slide_delay = t.slide_delay;
    x.slide_frames = (uint32_t(t.slide_length) * frames) >> 8;
    if (t.slide_flags & 2)
    {
        x.slide_end = int32_t(target - x.base_pitch);
    }
    else
    {
        x.slide_end = int32_t(x.base_pitch - target);
        x.base_pitch = target;
    }
    x.slide_step = SignedDiv(x.slide_end, int32_t(x.slide_frames));
    if (t.slide_flags & 4)
    {
        t.slide_note = note;
    }
    else
    {
        t.slide = 0;
    }
    x.slide = 0;
}

// Sets what the voice plays: a sample, a wave, a table of duties or widths, or one duty or width. One duty or width
// replaces the low byte of `x.psg`.
void Sequencer::SetVoiceSound(const Track& t, Voice& x, const Lookup& lk)
{
    const uint32_t region = lk.region;
    if (x.type == kSampleType)
    {
        const uint32_t index = region ? rom_.U16(region + 2) : key_sample_;
        x.sample = t.samples + rom_.U32(t.samples + 4 * index);
    }
    else if (x.type == kWaveType)
    {
        x.psg = lk.wave;
    }
    else if (HasPsgTable(rom_, region))
    {
        x.psg = lk.psg_table;
    }
    else
    {
        x.psg = (x.psg & ~0xFFu) | (rom_.U16(region + 2) & 0xFFu);
    }
}

} // namespace ntdlib::rd2
