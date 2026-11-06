// SPDX-License-Identifier: MIT

// Locating the Konami sound driver's tables inside a ROM.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/seqformat.h"

namespace ntdlib::konami
{

// A DirectSound sample: header { u32 step, s32 length, s32 loop_start } + s8 PCM. In the Dungeon Dice Monsters
// revision, an entry of the sample table points at the PCM, and gives the length and the sample's rate. The later
// builds of the Ultimate Masters revision can also pack a sample. Bit 31 of its length is then set, and a table of 32
// pairs of samples comes between the header and the packed data.
struct SampleInfo
{
    // Returns true if the sample loops: its loop start is inside it.
    bool Looped() const
    {
        return loop_start >= 0 && loop_start < length;
    }

    // Returns the sample's playback rate in Hz at semitone 0, with the mixer running at `mix_rate`.
    double Rate(double mix_rate) const
    {
        return period ? double(agb::kSecondCycles) / period : mix_rate * step / 4096.0;
    }

    bool valid = false;
    uint32_t step = 0;       // playback step at semitone 0, 4.12 fixed point per mixer sample
    uint32_t period = 0;     // Dungeon Dice Monsters: CPU cycles per sample at the sample's rate
    int32_t length = 0;      // in samples (bytes)
    int32_t loop_start = -1; // -1: one-shot
    uint32_t data = 0;       // address of the first PCM byte, or of the packed data
    bool packed = false;     // true if the data is packed (see SamplePcm)
};

// Returns the `length` samples of `s`'s PCM, unpacked if the sample is packed.
std::vector<int8_t> SamplePcm(const Rom& rom, const SampleInfo& s);

// Returns entry `index` of the table of halfwords at `table`. Returns nothing if `table` is 0 (not found) or if the
// entry lies outside the ROM. A negative index reads before the table. The driver reads there too.
std::optional<uint16_t> TableEntry(const Rom& rom, uint32_t table, int index);

// Driver table addresses, detection details (`log`), and problems or assumptions made during detection (`warnings`).
struct DriverInfo
{
    // Returns the header of sample `index`, with valid = false if the table entry or header is unusable. A sample
    // number with bit 15 set is sample `index & 0xFFF` of `live_sample_table`.
    SampleInfo Sample(const Rom& rom, int index) const;

    // Returns the level that the volume table gives a voice's side at `volume` and that side's pan level `pan`. The pan
    // level is 0..63 in the Ultimate Masters revision and 0..15 in the older ones, whose PSG output uses the table too.
    int TableLevel(const Rom& rom, int volume, int pan) const;

    // Returns the mixer level of a voice's side at `volume` and that side's pan level `pan`, in the Ultimate Masters
    // revision's units: a level of n plays a full-scale sample at amplitude n/128.
    int VoiceLevel(const Rom& rom, int volume, int pan) const;

    // Returns the noise table's word for a noise note: the NR43 value in the low byte, and the restart bit (15). The
    // older revisions read the table at the noise track's pitch, the note × 32.
    uint16_t NoiseSetting(const Rom& rom, int note) const;

    Revision revision = Revision::kUltimateMasters;
    uint32_t song_table = 0;
    int song_count = 0;
    uint32_t sample_table = 0;
    uint32_t volume_table = 0;   // u8 [volume][64 pan levels], or [volume][16] in older revisions; 0 = not found
    uint32_t psg_freq_table = 0; // u16 GB frequency per 1/32 semitone, or in Dungeon Dice Monsters per 1/16 semitone of
                                 // notes 0-71, then noise settings and a vibrato part; 0 = not found
    uint32_t noise_table = 0;    // u16 NR43 (+flags) per noise note; 0 = not found
    uint32_t wave_table = 0;     // 16-byte wave RAM images, [wave][16 volumes], or in Dungeon Dice Monsters [wave] at
                                 // full scale; 0 = not found
    uint32_t sfx_wave_table = 0; // the same for waves 80 and up (wave & 7F), which sound effects use; 0 = not found
    uint32_t timer_table = 0;    // u32 timer 0 setting per mixer mode; 0 = not found
    double mix_rate = 0;         // DirectSound mixer output rate in Hz; 0 in Dungeon Dice Monsters, which has no mixer

    // The Dungeon Dice Monsters revision's other tables: the sample of each sample map entry that a note gives, the
    // timer period of each sample pitch, and the NR32 volume code of each track volume. Its vibrato reads the PSG
    // frequency table from entry `vibrato_entry` on, 4 entries for each note.
    uint32_t sample_map = 0;          // u8 sample per entry
    uint32_t sample_period_table = 0; // u16 CPU cycles per sample, per 1/16 semitone; 0 for a sample's rate
    uint32_t wave_volume_table = 0;   // u16 NR32 value per volume (0-15)
    int vibrato_entry = 0;

    // True if a pitch bend (F2) also bends the notes after it, as it does in Yu-Gi-Oh! Ultimate Masters Edition. Other
    // builds of the driver, such as the Shaman King games', bend only the note that's playing, until its next note or
    // vibrato step.
    bool bend_kept = true;

    // The songs that sound different in the game's live stage. Each song request sets FA's setting to FF and gives F6
    // no table of samples. The live stage sets the setting to 0 after its request, and gives some songs a table. The
    // game plays these songs in both states. Each of them therefore has a second version. Only BMAJ and B3MJ have any.
    std::vector<int> live_songs;

    // The table of samples that the live stage gives F6 for each song that has one: entry 0 is 0, and the entries
    // after it hold sample numbers with flags (see Request::samples).
    std::map<int, std::vector<uint16_t>> live_tables;

    // The second sample table. The note start reads it for a sample number with bit 15 set. Only the samples of the
    // live stages' tables are there. 0 if the game has no live stage.
    uint32_t live_sample_table = 0;

    std::vector<std::string> log;
    std::vector<std::string> warnings;
};

// Table addresses and settings to use instead of detecting them; 0 means detect.
struct DriverOverrides
{
    uint32_t song_table = 0;
    int song_count = 0;
    uint32_t sample_table = 0;
    double mix_rate = 0;
};

// Finds the driver's tables in `rom`. Returns false and sets `error` if the driver's tables are unusable, or missing
// although its command reader is there, or returns false with `error` empty if the game shows no sign of the driver.
bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error);

} // namespace ntdlib::konami
