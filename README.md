# ntdlib

ntdlib is a C++20 library for the formats and hardware of Nintendo's consoles: NintendoWare's sound archives, the
consoles' wave encodings, the PowerPC and the DSP of the GameCube, the Wii and the Wii U, GameCube and Wii disc
images, the sound drivers of the Game Boy Advance, the DS's cartridge images, sound archives and sound drivers, and
the Nintendo 64's sound files. It also models NintendoWare's sequence player, and converts Nintendo's sound data to
MIDI, SoundFont 2 and SFZ files.

ntdlib builds on xsflib, a library for players and rippers of the PSF family of formats: byte order, bounds-checked
reads, the PSF container and playback. ntdlib's modules are static libraries, apart from `ntdlib::nwsnd_archive` and
`ntdlib::nwsnd_lengths`. Those two hold only headers. A project links the modules it uses.

| Module | What it holds |
|---|---|
| `ntdlib::commands` | A common form for the commands of the consoles' sequences, with their values in units that are the same on every console |
| `ntdlib::common` | The consoles' address types, the PowerPC's negated fused multiply-adds, the PowerPC's and ARM's conversions of a float to a 32-bit integer, AArch64's floating-point minimum and maximum and its reciprocal's estimate and step, DOL executables, REL modules, the builds of the Wii's sound code that a main.dol links, a decoder of the DEFLATE and zlib streams in GSF files and Rare's N64 blocks, the decoders of Yay0 and Yaz0, the music that a game plays as several sequences together (`ntdlib/sound_pieces.h`), the length of a held note (`ntdlib/held_notes.h`), and the rounding of a value up to a multiple and of a float to the nearest 32-bit integer (`ntdlib/rounding.h`) |
| `ntdlib::ppc` | A PowerPC interpreter for the GameCube, the Wii and the Wii U |
| `ntdlib::dsp` | The DSP of the GameCube, the Wii and the Wii U, with each console's memory, and the accelerator's reads and the accumulators' helpers for models of a DSP program |
| `ntdlib::flipper` | The devices that the GameCube and the Wii share (the DSP interface, the audio interface, EXI, a disc of files and the hardware page's shared blocks), and the parts of a machine for game mode that don't depend on its other devices, with a snapshot of the devices that both consoles have. Also the host's side of the Z2 start-up's parameters and the wave list that a rip writes into a JAudio driver's parameters |
| `ntdlib::ax` | What the Wii's and the Wii U's AX libraries share: the constants, the voice allocation and the low-pass coefficients |
| `ntdlib::disc` | GameCube and Wii disc images: ISO, NKit, WBFS and WDF, and extracted discs. With `GameOptions::titles`, also the Wii's titles, such as WiiWare games, from a folder with their TMD and decrypted contents. Titles and file names come out in UTF-8. They're decoded from Shift-JIS, or from Windows-1252 for the title of a disc from outside Japan. With `GameOptions::cpk_sound_archives`, also the sound archives inside CRI's CPK archives, stored as they are or compressed with CRILAYLA (`ntdlib/disc/cpk.h`). Also FSYS archives, with LZSS, the compression of their files (`ntdlib/disc/fsys.h`). Pokémon Colosseum keeps most of its files in FSYS archives. It requires no other library |
| `ntdlib::disc_containers` | The containers that store a disc image compressed or in sparse blocks: GCZ, CISO, WIA and RVZ, opened through `ntdlib::disc` |
| `ntdlib::sar` | NintendoWare's sound archives for the 3DS (CSAR), the Wii U and the Switch (FSAR), and their parts. The rules of a game's archive give what the game's code does with its sounds: the tracks it turns down and the reverbs it puts on aux bus B |
| `ntdlib::sar_layout` | A sound archive of the 3DS, the Wii U or the Switch cut into pieces for an editor: its sequence files, decoded into the form of `ntdlib::commands`, and the runs of bytes between them. Joining the pieces gives the archive back byte for byte. After an edit, joining lays the pieces out again and writes the archive's offsets and sizes. WriteTableValues writes new values into the archive's tables. TableValuePlaces finds which of a sound's optional values the tables hold. Also readers and writers of the sequence files (CSEQ and FSEQ), the wave files (CWAV and FWAV), the wave archives (CWAR and FWAR), the STRG block and the INFO block. The STRG block holds the names of the archive's items, and the tree that finds an item by its name. The INFO block holds the tables of the archive's items. WithTables puts new STRG and INFO blocks in an archive. WithNewFile and WithNewSequenceSound add a file and a sequence sound to it. ReadArchiveModel reads an archive of the 3DS, the Wii U or the Switch as the fields of its items, and BuildArchive lays an archive out from them as NintendoWare's tools do |
| `ntdlib::rsar` | NintendoWare's sound archives for the Wii (RSAR), and their parts. Also the reader and the writer of the stream files beside them (RSTM), the decoding of their channels, streams made from samples, and the sizes that the file table gives the files beside an archive |
| `ntdlib::rsar_layout` | A sound archive of the Wii (RSAR) cut into pieces for an editor by the rules of `ntdlib::sar_layout`: its sequence files (RSEQ), decoded into the form of `ntdlib::commands`, and the runs of bytes between them. Joining the pieces gives the archive back byte for byte. After an edit, joining lays the pieces out again and writes the archive's offsets and sizes. Also the readers and writers of the sequence files, the waves (RWAV), the wave archives (RWAR), the banks (RBNK) and the wave sound files (RWSD). The writers lay the files out as NW4R's tools do. ReadArchiveModel reads an archive of the Wii as the fields of its items, and BuildArchive lays an archive out from them as the tools do. Also the Wii's banks, waves and streams as files of the 3DS, the Wii U or the Switch, and the later consoles' banks, waves and streams as files of the Wii |
| `ntdlib::musyx` | The readers of Factor 5's MusyX data for the GameCube: a project's groups with their pages, MIDI setups and sound effects, a pool's macros, tables, keymaps and layers, a sample directory with the decoding of its samples, and songs |
| `ntdlib::musyx_catalogue` | The catalogues of the MusyX games whose code keeps its sounds in other layouts: the file sets, the songs and the sounds, read from a game's files and code. The readers unpack Retro Studios' packs and Yay0 data, and read FSYS archives and modules of code. Built with `NTDLIB_DISC` |
| `ntdlib::jaudio` | The readers of JAudio2's sound data: the start-up archive (BAA) with its sound table, sequence collection, wave systems (WSYS) and banks (IBNK), and RARC archives, compressed with Yaz0 or not, and refilled from another release's sequence table. The readers of JAudio1's start-up archive (AAF): its sound table, its wave systems, the effects' slots in its table of scenes, and the sequence index of an earlier JAudio1. The readers of Pikmin's bank file (.bx), and of the sequence index (BARC) that Pikmin keeps in its executable, with a search for the index there. The readers take the little-endian files of the Switch's releases, with the 64-bit offsets of Pikmin's. Also the length analysis of a sound in game mode, for JAudio2 and JAudio1 |
| `ntdlib::jaudio2` | A model of JAudio2 in the Switch ports of the Super Mario Galaxy games, and of Hagi, the emulator of the GameCube's and the Wii's hardware that each port's `main` links. Hagi runs JAudio's DSP program as a high-level emulation. The model follows that emulation for the stereo output: AFC, PCM8 and PCM16 waves, their resampling, the oscillators, filters and mixers, the four FX lines, the matrix of the buses, and Hagi's conversion to 48,000 Hz. Above it, JAudio2's arithmetic as the ports build it for AArch64, its envelopes and LFOs, banks, channels and sequences, and an engine that plays a sound of the start-up archive's sound table. The game's code above JAudio2 sets the engine's levels, FX lines and track states |
| `ntdlib::jaudio1` | A model of JAudio1 in the Switch port of Pikmin 2, on the model of Hagi's DSP program: its arithmetic, envelopes and LFOs, banks, channels and sequences, the conductor of the cave music, and an engine that plays the game's music and effects, with the answers that the game's code gives its sequences |
| `ntdlib::jac` | A model of the C JAudio (Jac) in the Switch port of Pikmin, on the model of Hagi's DSP program: its arithmetic, envelopes and LFOs, the bank with its wave scenes in ARAM, channels and sequences, and the game's code that plays its music, effects and cutscenes' sounds |
| `ntdlib::n64` | The Nintendo 64's sound files: the readers and writers of libultra's sequence banks (S1) and bank files (B1), of standard and compressed MIDI, of VADPCM samples, and of the banks of Nintendo EAD's sound engines, with the reader of those engines' sequences. A file comes back byte for byte. Compressed MIDI files can be built from events, with new blocks. VADPCM frames come from samples by the rule of the SDK's encoder. Frame-by-frame models of the console's sequence players and audio microcodes run them, with snapshots, mutes, meters and an enhanced mixing mode |
| `ntdlib::agb` | What the drivers of the Game Boy Advance share: the reader of ROMs and of GSF sets, the scanners of Thumb and ARM code that find a driver, the GBA's constants, and the plan of a song's conversion from its tracks' loops and ends |
| `ntdlib::agb_apu` | A model of the GBA's sound hardware: the Game Boy's four PSG channels, the two DirectSound FIFOs that the timers empty, and the output stage with its bias and resolution. The model runs on the CPU's cycles and takes the stores to the sound registers between runs. Its enhanced mixing mode puts out floats at about 48,022 Hz, without the output stage's limits: the PSG's channels through band-limited steps, and FIFOs of floats |
| `ntdlib::mp2k` | MusicPlayer2000 (MP2K), the sound driver of Nintendo's GBA SDK: its detection in a ROM, the readers of its songs and voice groups, and a model of its sequencer and its mixer. The model mixes the DirectSound channels point for point as the SDK's stereo and mono mixers do, with their reverb. It also mixes them as Camelot's mixer does, with its echo and its synth voices. Its enhanced mixing mode mixes the same channels, reverb and echo in floating point at about 48,022 Hz, through the band-limited interpolator. It makes the driver's writes to the PSG's registers too. Cut and Join cut a ROM's songs into pieces for an editor, by the rules of `ntdlib::sar_layout`, and lay them out again. A song that outgrows its place moves to the free space at the ROM's end |
| `ntdlib::mp2k_player` | A player of MP2K's songs on the models of the driver and the sound hardware (`ntdlib::mp2k` and `ntdlib::agb_apu`), a frame at a time, with frames without mixing, snapshots, mutes and meters of the tracks, the command behind each note, and the enhanced mixing mode. `ntdlib/mp2k/archive_source.h` gives xsflib's archive mode its steps |
| `ntdlib::konami` | Konami's GBA sound driver: its detection in a ROM, the reader of its songs, and a model of its sequencer, its output stage and its mixers. The model mixes the DirectSound voices point for point as each revision of the driver does, with its echo, and makes the driver's stores to the PSG's registers. The revision without a mixer runs on its FIFOs' timers. An enhanced mixing mode mixes the same voices and echo in floating point at about 48,022 Hz, through the band-limited interpolator. Cut and Join cut a ROM's songs into pieces for an editor, and lay them out again |
| `ntdlib::konami_player` | A player of the songs of Konami's driver on the models of the driver and the sound hardware (`ntdlib::konami` and `ntdlib::agb_apu`), a frame at a time, with frames without samples, snapshots, mutes and solos of the tracks, meters of the tracks and the output, and the enhanced mixing mode. `ntdlib/konami/archive_source.h` gives xsflib's archive mode its steps |
| `ntdlib::rare` | Rare's GBA sound driver: its detection in a ROM, the tunes' commands and instruments, the decoder and the encoder of its track commands in the form of `ntdlib::commands`, the cut and the join of a ROM's tunes for an editor, and a model of the routine that the driver runs each frame |
| `ntdlib::rare_player` | A player of the tunes of Rare's driver on the models of the driver and the sound hardware (`ntdlib::rare` and `ntdlib::agb_apu`), a frame at a time, with snapshots, mutes and meters of the tracks, and an enhanced mixing mode |
| `ntdlib::quintet` | Quintet's GBA sound driver: its detection in a ROM, the songs' commands and instruments, the decoder and the encoder of its channel commands in the form of `ntdlib::commands`, the cut and the join of a ROM's songs for an editor, and a model of the driver's play routine and its writes to the sound registers |
| `ntdlib::quintet_player` | A player of the songs of Quintet's driver on the models of the driver and the sound hardware (`ntdlib::quintet` and `ntdlib::agb_apu`), a frame at a time, with snapshots, mutes and meters of the tracks, and an enhanced mixing mode |
| `ntdlib::krawall` | Krawall, a player of XM and S3M modules for the GBA: the detection of its builds in a ROM, the modules' patterns, the decoder and the encoder of their commands, the cut and the join of a ROM for an editor, the instruments and samples, and a model of the player and its mixer |
| `ntdlib::krawall_player` | A player of a ROM's modules on the models of Krawall and the sound hardware (`ntdlib::krawall` and `ntdlib::agb_apu`), a frame at a time, with snapshots, mutes and meters of the tracks, and an enhanced mixing mode |
| `ntdlib::rd2` | Nintendo R&D2's GBA sound driver, the one of The Legend of Zelda: A Link to the Past & Four Swords and the Super Mario Advance games: its detection in a ROM, the reader of its songs, and a model of its sequencer and its mixer. The model mixes the sample voices point for point as each revision of the driver does, with its echo, and makes the driver's stores to the PSG's registers. An enhanced mixing mode mixes the same voices and echo in floating point at about 48,022 Hz, through the band-limited interpolator. `ntdlib/rd2/commands.h` decodes and encodes a sequence's commands, and `ntdlib/rd2/layout.h` cuts a ROM's sequences into pieces for an editor and lays them out again |
| `ntdlib::rd2_player` | A player of the songs of R&D2's driver on the models of the driver and the sound hardware (`ntdlib::rd2` and `ntdlib::agb_apu`), a frame at a time, with frames without samples, snapshots, mutes and solos of the tracks, meters of the tracks and the output, and the enhanced mixing mode. `ntdlib/rd2/archive_source.h` gives xsflib's archive mode its steps |
| `ntdlib::brownie` | Brownie Brown's GBA sound driver: its detection in a ROM, the reader of its sounds, and a model of its per-frame routine and of how its samples play. The Sword of Mana revision's model mixes its 5 voices point for point. The Magical Vacation revision's model plays a sample on each FIFO at its timer's rate. The model makes the driver's stores to the PSG's registers too. An enhanced mixing mode plays the same voices in floating point at about 48,022 Hz, through the band-limited interpolator. A decoder and an encoder cover the sounds' commands. Cut and Join cut a ROM's sounds into pieces for an editor, and lay them out again |
| `ntdlib::brownie_player` | A player of the sounds of Brownie Brown's driver on the models of the driver and the sound hardware (`ntdlib::brownie` and `ntdlib::agb_apu`), a frame at a time, with frames without samples, snapshots, mutes and solos of the tracks, meters of the tracks and the output, and the enhanced mixing mode. `ntdlib/brownie/archive_source.h` gives xsflib's archive mode its steps |
| `ntdlib::ubimilan` | Ubisoft Milan's GBA sound driver: its detection in a ROM, the reader of its pieces, and a model of its sequencer and its mixer. The model mixes the DirectSound voices point for point as the driver's mixer does, and makes the driver's stores to the PSG's registers. An enhanced mixing mode mixes the same voices in floating point at about 48,022 Hz, through the band-limited interpolator. Cut and Join cut a ROM's sequences into pieces for an editor, and lay them out again |
| `ntdlib::ubimilan_player` | A player of the pieces of Ubisoft Milan's driver on the models of the driver and the sound hardware (`ntdlib::ubimilan` and `ntdlib::agb_apu`), a frame at a time, with frames without samples, snapshots, mutes and solos of the tracks, meters of the tracks and the output, and the enhanced mixing mode. `ntdlib/ubimilan/archive_source.h` gives xsflib's archive mode its steps |
| `ntdlib::rd1` | Nintendo R&D1's GBA sound driver for Standard MIDI Files, the one of WarioWare, Inc.: Mega Microgame$! and Rhythm Tengoku: its detection in a ROM, the songs' records and files, the instrument records, and a model of a player's tracks and channels. `ntdlib/rd1/commands.h` decodes and encodes a file's events, and `ntdlib/rd1/layout.h` cuts a ROM's files into pieces for an editor and lays them out again |
| `ntdlib::rd1_player` | A player of the songs of R&D1's driver on the game's copy of the driver (`ntdlib::rd1_game`) and the model of the sound hardware (`ntdlib::agb_apu`), a frame at a time, with frames without samples, snapshots, auditions of a bank's programs, mutes and solos of the tracks, meters of the tracks and the output, and an enhanced mixing mode. The enhanced mode mixes the driver's voices, its echo and its filter in floating point at about 48,022 Hz, through the band-limited interpolator. `ntdlib/rd1/archive_source.h` gives xsflib's archive mode its steps |
| `ntdlib::alphadream` | AlphaDream's GBA sound driver, the one of Mario & Luigi: Superstar Saga: its detection in a ROM, in three versions, and the readers of its songs, instruments and samples. `ntdlib/alphadream/commands.h` decodes and encodes a song's commands, and `ntdlib/alphadream/layout.h` cuts a ROM's songs into pieces for an editor and lays them out again |
| `ntdlib::alphadream_player` | A player of the songs of AlphaDream's driver on the game's copy of the driver (`ntdlib::alphadream_game`) and the model of the sound hardware (`ntdlib::agb_apu`), a frame at a time, with frames without samples, snapshots, auditions of the instruments, mutes and solos of the tracks, meters of the tracks and the output, and an enhanced mixing mode. The enhanced mode mixes the driver's sample channels in floating point at about 48,022 Hz, through the band-limited interpolator. `ntdlib/alphadream/archive_source.h` gives xsflib's archive mode its steps |
| `ntdlib::agb_system` | Enough of a GBA to run a game's sound driver on ntdlib's ARM interpreter: the work memories, the cartridge, the I/O registers, the DMA's immediate transfers and the BIOS's calls of division, copying and LZ77 decompression. It links `ntdlib::agb` and `ntdlib::arm` |
| `ntdlib::mp2k_game` | A game's MP2K driver, run on `ntdlib::agb_system`: its init, its start of a song and its frames, with the mixer's output and the channels that each frame leaves. It's the reference for the model of the driver |
| `ntdlib::konami_game` | A game's copy of Konami's driver, run on `ntdlib::agb_system`: its init, its request of a song and its frames, with its stores to the sound registers and the bytes that its mixers give the FIFOs. It's the reference for the model of the driver |
| `ntdlib::rare_game` | A game's copy of Rare's driver, run on `ntdlib::agb_system`. It's the reference for the model of the driver |
| `ntdlib::quintet_game` | A game's copy of Quintet's driver, run on `ntdlib::agb_system`. It's the reference for the model of the driver |
| `ntdlib::krawall_game` | A game's copy of Krawall, run on `ntdlib::agb_system`. It's the reference for the model of the player |
| `ntdlib::rd2_game` | A game's copy of R&D2's driver, run on `ntdlib::agb_system`: its init, its request of a song and its frames, with its stores to the sound registers and the bytes that its mixer gives the FIFOs. It's the reference for the model of the driver |
| `ntdlib::brownie_game` | A game's copy of Brownie Brown's driver, run on `ntdlib::agb_system`: its init, its request of a sound and its frames, with its stores to the sound registers and the bytes that its interrupts give the FIFOs. It's the reference for the model of the driver |
| `ntdlib::ubimilan_game` | A game's copy of Ubisoft Milan's driver, run on `ntdlib::agb_system`: its start of a piece and its frames, with its stores to the sound registers and the bytes that its mixer gives the FIFOs. It's the reference for the model of the driver |
| `ntdlib::rd1_game` | A game's copy of R&D1's driver, run on `ntdlib::agb_system`: its init, the start of a song, the FIFOs' transfers and its frames, with its stores to the sound registers, the bytes that its mixer gives the FIFOs, and a hook before each frame's mix |
| `ntdlib::alphadream_game` | A game's copy of AlphaDream's driver, run on `ntdlib::agb_system`: the mixer's setup and the driver's init, the start of a song, VBlank's swap of the mixer's halves and its frames, with its stores to the sound registers, the halves that its mixer fills, the sample channels' records, its step routine, and a hook before each frame's mix |
| `ntdlib::wave` | Decoders of DSP-ADPCM (the DSP accelerator's step among its forms), IMA-ADPCM, the GameCube's AFC, PCM8 and PCM16, and the decoding of a sound archive's waves. An encoder of DSP-ADPCM, with the decoder's state at a loop's start, and the move of a loop's start to a frame. An encoder of IMA-ADPCM. A band-limited interpolator of a channel's samples (`ntdlib/wave/sinc.h`), and band-limited steps for the channels of a sound chip (`ntdlib/wave/blep.h`) |
| `ntdlib::rip_archive` | Ripping the Wii U's and the Switch's sound archives into PSF sets: the sounds to rip, the data they require, the groups kept outside an archive, the part of an archive and of its group files that a library holds, the timing helpers, the states of a sound whose tracks a game's code turns up and down, and the libraries and minis that a rip writes. Rips of the 3DS's sound archives take the groups kept outside an archive, the part of an archive and of its group files, and the minis from here too. Built when xsflib has `xsflib::psf` |
| `ntdlib::bezel` | The reader of Bezel Engine's data sheets (ELF relocatable objects), and the levels at which a game's sound sheets play its sounds |
| `ntdlib::nwsnd` | The tables of NintendoWare's sound libraries, and a model of their sequence player and channels for the Wii, the 3DS, the Wii U and the Switch, and of the Wii U's and the Switch's voices. The player reads its commands through the decoder of `ntdlib::nwsnd_commands`, which the target holds |
| `ntdlib::nwsnd_archive` | The banks, wave archives and waves that the model's engine loads for its notes from a sound archive of the Wii U or the Switch, read with `ntdlib::sar`. The Wii's model uses `ntdlib::nwsnd` without it |
| `ntdlib::nwsnd_commands` | A decoder and an encoder of the bytecode of NintendoWare's sequences in the form of `ntdlib::commands`, and the units of each console's values. The encoder gives back the bytes that the decoder read. `ntdlib::nwsnd` holds them, and this target links it |
| `ntdlib::nwsnd_lengths` | The length analysis of a sequence sound of the Wii, the 3DS, the Wii U or the Switch on the model of the console's sound library, with xsflib's analysis: how long the sound plays before its fade, and how long the fade lasts. A program links `ntdlib::rvl_snd`, `ntdlib::ctr`, `ntdlib::cafe` or `ntdlib::nx` with it |
| `ntdlib::nx` | A model of the Switch's audio renderer (`nn::audio`) with its reverb, its I3DL2 reverb, its delay and its light limiter, and archive mode's model of `nn::atk` on it: an engine that plays a sound archive's sequences, wave sounds and stream sounds as the Switch does, without the game's code. Also the system's downmix of 6 channels to stereo, and the reader of the Switch's executables (NSO), with a decoder of the LZ4 blocks that store their segments |
| `ntdlib::nx_bex` | Bezel Engine's music sync on the Switch: the reader of a stream's regions, markers and jump setting, and the model that follows the stream's beats and chords for the sequences that read them through user procedures. Also the models of two games' other user procedures: random draws, counters and a table |
| `ntdlib::rvl_ax` | The CPU side of the Wii's AX: the voices and their priorities, the copy of the voices' parameter blocks into main memory each frame, the depop block, the command list that the DSP microcode mixes, and the mail exchange with the microcode on `ntdlib::dsp`. It follows the builds of October and December 2006. A model of the microcode of December 2006 mixes the command list without the DSP, sample for sample as the microcode does. The model also takes the command lists and the parameter blocks of the build of October 2006, and mixes them sample for sample as that build's microcode does |
| `ntdlib::rvl_axfx` | Models of the effects that the Wii's SDK runs on AX's aux buses (AXFX): the high-quality and the standard reverb, the chorus and the delay, in the builds of October 2006, December 2006 and September 2008, and the chorus and the extended delay of February 2009. The Photo Channel 1.1's mood effect is modelled too. Each model gives the same samples as the games' code |
| `ntdlib::rvl_snd` | NW4R SND's engine on the Wii's AX: the voices, the channels, the banks, the players of sequences, wave sounds and stream sounds, and the effects, for archive mode. It plays a sound archive's sequences, wave sounds and stream sounds as the Wii does, on the game's microcode or on the model of the microcode |
| `ntdlib::rvl_nx` | NW4R SND's engine on the layer that The Legend of Zelda: Skyward Sword HD puts in place of the Wii's AX: AX's voices mixed by the model of the Switch's audio renderer at 32 kHz, then upsampled to 48 kHz as the SDK does. It plays the game's sound archive for archive mode, with the game's reverbs on the aux buses and its mix for Dolby Pro Logic II. The system's downmix then turns the 6 channels into stereo |
| `ntdlib::cafe` | A model of the Wii U's AX and of the effects that its system library runs on AX's aux buses (AXFX), Nintendo Land's effect classes, and archive mode's model of `nw::snd` on them: an engine that plays a sound archive's sequences, wave sounds and stream sounds as the Wii U does, without the game's code. The renderer follows the DSP microcode that the system's sound library uploads |
| `ntdlib::cafe_sequence_starts` | How known Wii U games start some sequences, and the later states that they give them, by the sound archive's SHA-1 and the sequence's label. A converter that runs no model of a game's code takes them from here |
| `ntdlib::ctr` | Archive mode's models of the 3DS's `nw::snd` and `nn::snd` on the 3DS's DSP: an engine that plays a sound archive's sequences, wave sounds and stream sounds as the 3DS does, without the game's code. The DSP is a model of the DSP's audio firmware, which requires no firmware image, or Teakra running a firmware that the program supplies, such as a game's `dspaudio.cdc`. The model gives the samples that Teakra gives. The package that `NTDLIB_INSTALL` installs leaves the module out |
| `ntdlib::ctr_fx` | Models of the effects that the 3DS's sound libraries run on the ARM11 over an aux bus: the reverb and the delay of nw::snd and of nn::snd, in both versions of nn::snd's reverb. Each model gives the same samples as the games' code. Also the ARM11's single-precision arithmetic in the mode that the 3DS's runtime sets, with the C library's `powf` in that mode |
| `ntdlib::ctr_starting_tracks` | The tracks that known 3DS games play as a stream sound or a sequence starts, by the sound archive's SHA-1 and the sound's name. A player that runs no model of the 3DS's library takes them from here |
| `ntdlib::ntr` | The DS's cartridge images (NDS ROMs): the header, the ARM9 and ARM7 binaries and their overlays, the DSi's binaries, and the file system. WithFiles writes an image again with new bytes for its files, as the SDK's tools lay an image out. In a DSi image, it also moves the digest and the DSi's part, and writes the digest's HMACs again with a key that the caller gives. It lists the hashes that the change leaves stale. Also the DS's sample rates and the IMA-ADPCM step of its sound unit |
| `ntdlib::ntr_snd` | A model of the DS's sound driver on the ARM7 (SND): its sequencer, its channels and its commands. The sequencer reads its sequences through the decoder of their bytecode, in the form of `ntdlib::commands`. The encoder comes with it |
| `ntdlib::sdat` | The DS's sound archives (SDAT) and their files: sequences (SSEQ), sequence archives (SSAR), banks (SBNK), wave archives (SWAR), waves (SWAV) and streams (STRM), with the decoding of their waves and streams. The writers lay each file out as the SDK's tools do. EncodeWave encodes samples into a wave of PCM8, PCM16 or IMA-ADPCM. ReadArchiveModel reads an archive as the fields of its items, and BuildArchive lays an archive out from them as the tools do |
| `ntdlib::sdat_cut` | A DS sound archive (SDAT) cut into pieces for an editor by the rules of `ntdlib::sar_layout`: its sequence files (SSEQ), decoded into the form of `ntdlib::commands`, and the runs of bytes between them. A sequence archive (SSAR) stays in a run. Joining the pieces gives the archive back byte for byte. After an edit, joining lays the pieces out again and writes the archive's offsets and sizes |
| `ntdlib::dse` | The files of Procyon Studio's sound driver for the DS (Digital Sound Elements, DSE): songs (smdl), wave banks (swdl) and banks of sound effects (sedl). The writers give a file back byte for byte, and lay a changed one out in the same way. Also a model of the driver's part on the ARM9: it plays a song, and gives each tick's commands for the ARM7's sound driver |
| `ntdlib::dse_layout` | A game's DSE files cut into pieces for an editor by the rules of `ntdlib::sar_layout`: the tracks of each song and sound effect, decoded into the form of `ntdlib::commands`, and the runs of bytes between them. Joining the pieces gives each file back byte for byte. After an edit, joining lays the pieces out again and writes the sizes, the offsets and the table of a bank of sound effects |
| `ntdlib::dse_player` | DSE's songs and sound effects on the models of DSE's driver, the DS's sound driver and the sound unit, with the steps of xsflib's archive mode |
| `ntdlib::dse_lengths` | The length analysis of DSE's songs and sound effects on the model of DSE's driver, with xsflib's analysis |
| `ntdlib::arm` | An interpreter of the DS's ARM7TDMI and ARM946E-S: the ARM and Thumb instruction sets, the exceptions, and the ARM946E-S's system control coprocessor with its tightly coupled memories |
| `ntdlib::ntr_arm7` | The ARM7's side of a DS: the CPU, the memories, the timers, the interrupts, the IPC and the sound unit, with a BIOS of high-level calls. It runs a cartridge's ARM7 binary with its sound driver |
| `ntdlib::ntr_machine` | A DS with both CPUs: the ARM9's side beside the ARM7's, with the card's reads from the image. It boots a cartridge image and runs the game's code |
| `ntdlib::ntr_driver_player` | Plays sounds on a cartridge's sound driver, which runs on `ntdlib::ntr_arm7`. The player takes the ARM9's part |
| `ntdlib::ntr_archive_player` | Plays a sound archive's sequences on the model of the DS's sound driver, with the ARM9's part on the host: the loading of the files, the commands of the SDK's sound archive player, and snapshots |
| `ntdlib::ntr_profiles` | What known DS games' code sets as it starts a sequence of their sound archives: the variables, the channels of the players and of some tracks, the capture units' reverb or output effect, and the driver's commands. Also the other states that the code gives a sequence while it plays. It links `ntdlib::ntr_archive_player` |
| `ntdlib::ntr_lengths` | The length analysis of a sound archive's sequence on the model of the DS's sound driver, with xsflib's analysis |
| `ntdlib::ntr_rip` | Ripping the DS's sound archives into PSF sets: the sequences to rip, the data they require, the part of an archive that a library holds, and the libraries and minis |
| `ntdlib::dse_rip` | Ripping DSE's songs and sound effects into PSF sets: the same parts as `ntdlib::ntr_rip` gives for sound archives |
| `ntdlib::sequence_export` | Conversion of NintendoWare's sound archives to MIDI, SoundFont 2 and SFZ files through seqlib. Also the command-line frame of a converter |
| `ntdlib::sequence_export_jaudio` | Conversion of JAudio's sound data to MIDI, SoundFont 2 and SFZ files, from a game's folder, disc image or title. A converter of NintendoWare's sound archives can take JAudio games with it too. It links `ntdlib::sequence_export`, `ntdlib::jaudio` and `ntdlib::disc` |
| `ntdlib::sequence_export_ntr` | Conversion of the DS's sound archives to MIDI, SoundFont 2 and SFZ files, on the model of the DS's sound driver. It links `ntdlib::sequence_export` and `ntdlib::ntr_archive_player` |
| `ntdlib::sequence_export_dse` | Conversion of DSE's songs and sound effects to MIDI, SoundFont 2 and SFZ files, on the models of DSE's driver and the DS's sound driver. It links `ntdlib::sequence_export` and `ntdlib::dse_player` |
| `ntdlib::sequence_export_n64` | Conversion of the Nintendo 64's sound data to MIDI, SoundFont 2 and SFZ files, from a cartridge image or an Aleck64 game's ROM set. It links `ntdlib::sequence_export`, `ntdlib::n64` and `xsflib::rom_set`. It builds only where xsflib builds `xsflib::rom_set`, with zlib |
| `ntdlib::sequence_export_agb` | What the conversions of the GBA's drivers share: the settings of a conversion and the names of its files, the SoundFont and MIDI writers on seqlib, the SFZ files made from a song's SoundFont, the banks of a song's programs, and the Game Boy PSG's voices. It links `ntdlib::agb` |
| `ntdlib::sequence_export_mp2k` | Conversion of MP2K's songs to MIDI and SoundFont 2 files. It links `ntdlib::sequence_export_agb` and `ntdlib::mp2k` |
| `ntdlib::sequence_export_konami` | Conversion of the songs of Konami's driver to MIDI and SoundFont 2 files, and in memory to SFZ files too. It links `ntdlib::sequence_export_agb` and `ntdlib::konami` |
| `ntdlib::sequence_export_rare` | Conversion of the tunes of Rare's driver to MIDI and SoundFont 2 files. It links `ntdlib::sequence_export_agb` and `ntdlib::rare` |
| `ntdlib::sequence_export_quintet` | Conversion of the songs of Quintet's driver to MIDI and SoundFont 2 files. It links `ntdlib::sequence_export_agb` and `ntdlib::quintet` |
| `ntdlib::sequence_export_krawall` | Conversion of Krawall's modules to MIDI and SoundFont 2 files. It links `ntdlib::sequence_export_agb` and `ntdlib::krawall` |
| `ntdlib::sequence_export_rd2` | Conversion of the songs of Nintendo R&D2's driver to MIDI and SoundFont 2 files. It links `ntdlib::sequence_export_agb` and `ntdlib::rd2` |
| `ntdlib::sequence_export_brownie` | Conversion of the sounds of Brownie Brown's driver to MIDI and SoundFont 2 files, and the record of a sound's performance: its stretches, the command behind each note and the changes of its mix. It links `ntdlib::sequence_export_agb` and `ntdlib::brownie` |
| `ntdlib::sequence_export_ubimilan` | Conversion of the pieces of Ubisoft Milan's driver to MIDI and SoundFont 2 files, and the record of a piece's performance, with the command behind each note. It links `ntdlib::sequence_export_agb` and `ntdlib::ubimilan` |
| `ntdlib::sequence_export_rd1` | Conversion of the songs of Nintendo R&D1's driver to MIDI and SoundFont 2 files, and the songs' performances for an editor. It links `ntdlib::sequence_export_agb` and `ntdlib::rd1` |
| `ntdlib::sequence_export_alphadream` | Conversion of the songs of AlphaDream's driver to MIDI and SoundFont 2 files, on a model of the driver that plays a song frame by frame, and the record of a song's performance: its stretches, the command behind each note and the changes of its mix. It links `ntdlib::sequence_export_agb` and `ntdlib::alphadream` |

`ntdlib::testing` holds builders of small synthetic sound archives, JAudio files, GBA cartridges, DS cartridge images,
DOL and NSO executables and other files for unit tests, the length analysis's cases for the tests of rip tools, the
signals and cases of a test of the 3DS's effects against the games' code, and checks of the files that
`ntdlib::sequence_export` writes. It also holds a test that plays a sound of a sound archive on
a model of a console's sound library, and a test of a disc's extracted files against their manifest.
Its guest calls a Wii game's functions on the Broadway interpreter, for tests that compare a model with the game's code.

`tools/sequence_export/filter_tables.py` derives the filter tables in `src/sequence_export/filter_tables_<console>.cpp`
from each console's reference game. For the Wii U and the Switch, it reads the game's tables in
`include/ntdlib/nwsnd/tables.h`. For the 3DS and the Wii, the user supplies the game's executable. The script requires
Python 3 and numpy.
`tools/sequence_export/midi_file.py` reads the Standard MIDI Files that the converters write, for Python checks
and research tools. It requires only Python 3.
`tools/build_ppc_driver.py` builds a rip format's game-mode driver for the GameCube or the Wii, and writes it as a C++
header. It requires a clang with the PowerPC target, such as LLVM's releases or Homebrew's llvm. Apple's clang lacks
that target. A driver can include the shared sources in `drivers/`. `drivers/jaudio/z2.h` holds the start-up of Z2,
the sound layer that Twilight Princess puts on JAudio2. A driver supplies the console's steps, and
`include/ntdlib/flipper/z2_params.h` gives the host's side of its parameters. The parameters of other JAudio games'
drivers hold the same list of wave groups (`include/ntdlib/flipper/wave_list.h`).
`tools/write_cp932_table.py` writes `src/disc/cp932.h`, the table of Shift-JIS's double-byte characters in code page
932, from Unicode's CP932.TXT. The user downloads that file. Building ntdlib requires none of these scripts.

## Building

ntdlib requires CMake 3.20 or later, a C++20 compiler and xsflib. xsflib is a Git submodule at `externals/xsflib`.
Clone with `git clone --recurse-submodules`, or run `git submodule update --init --recursive` in an existing clone.
The xsflib submodule holds seqlib as a submodule too.

ntdlib supports macOS on Apple Silicon and Windows x64 built with clang-cl. Other platforms and compilers aren't
supported.

- **Windows x64**: Visual Studio 2022 with the C++ workload and its "C++ Clang tools for Windows" component. Configure
  with `cmake -S . -B build -A x64 -T ClangCL` in place of the first command below.
- **macOS on Apple Silicon**: Xcode's command-line tools (`xcode-select --install`) and CMake (from cmake.org or
  Homebrew).

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The compressed containers of disc images require zlib, Zstandard, liblzma and bzip2. xsflib finds them. An installed
copy of each library is used if CMake finds one. Otherwise CMake downloads and builds it.

Output is meant to be the same on both platforms. ntdlib builds with xsflib's flags. They turn off the contraction of
floating-point operations into fused multiply-adds. A project that includes ntdlib's headers should build its sources
the same way.

## Options

| Option | Default | Effect |
|---|---|---|
| `NTDLIB_DISC` | on when built alone | `ntdlib::disc_containers`, the compressed containers of disc images, and `ntdlib::musyx_catalogue`. Requires zlib, Zstandard, liblzma and bzip2. |
| `NTDLIB_SEQUENCE_EXPORT` | off | `ntdlib::sequence_export` and the `ntdlib::sequence_export_*` modules, the conversion to MIDI, SoundFont 2 and SFZ. Requires seqlib. |
| `NTDLIB_CTR` | on when built alone | `ntdlib::ctr`, the 3DS's DSP in Teakra and archive mode's model on it. Off with `NTDLIB_INSTALL`. A project that turns it on and adds Teakra too adds Teakra first. |
| `NTDLIB_TESTS` | on when built alone | The tests, registered with CTest. |
| `NTDLIB_INSTALL` | off | Installs the headers, the libraries and the CMake package. A build of ntdlib alone installs its copy of xsflib too. Requires installed copies of the libraries above. |

## Using ntdlib

Each module is a static library with an alias, such as `ntdlib::sar`. A project links the modules it uses. The
helpers for unit tests are `ntdlib::testing`.

A project that uses ntdlib adds xsflib first. ntdlib then uses that copy of xsflib. The copy in ntdlib's
submodule goes unused. A project that turns on `NTDLIB_DISC` also sets `XSFLIB_DISC_LIBRARIES` before it adds xsflib.
A build of ntdlib alone sets it for its submodule.

As Git submodules:

```cmake
set(XSFLIB_DISC_LIBRARIES ON)
add_subdirectory(externals/xsflib EXCLUDE_FROM_ALL)
add_subdirectory(externals/ntdlib EXCLUDE_FROM_ALL)
target_link_libraries(my_program PRIVATE ntdlib::sar)
```

With FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(xsflib GIT_REPOSITORY <xsflib's URL> GIT_TAG <a release tag>)
FetchContent_Declare(ntdlib GIT_REPOSITORY <ntdlib's URL> GIT_TAG <a release tag>)
FetchContent_MakeAvailable(xsflib ntdlib)
target_link_libraries(my_program PRIVATE ntdlib::sar)
```

From installed copies (`cmake --install build` after configuring each library with its `_INSTALL` option), with xsflib
installed first:

```cmake
find_package(ntdlib 3.0 REQUIRED)
target_link_libraries(my_program PRIVATE ntdlib::sar)
```

ntdlib's package finds xsflib, and seqlib with `NTDLIB_SEQUENCE_EXPORT`. A project that uses
`ntdlib::sequence_export` adds seqlib first. Then it sets `NTDLIB_SEQUENCE_EXPORT` and adds xsflib and ntdlib.

## Licence

ntdlib is under the MIT licence (`LICENSE`). One file, `src/disc/cp932.h`, contains data from Unicode and is under the
Unicode License v3. Another, `src/common/inflate.cpp`, is adapted from Mark Adler's puff and is under the zlib licence.
`THIRD-PARTY-NOTICES.md` gives both licences' texts. It also lists the libraries that ntdlib uses and their licences,
and the documentation that parts of ntdlib were written from.

`externals/teakra` holds a copy of Teakra with changes. That copy stays under Teakra's MIT licence. The copy of Catch2
inside it, for Teakra's unit tests, is under the Boost Software License 1.0. `THIRD-PARTY-NOTICES.md` gives both
licences' texts.
