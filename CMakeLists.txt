# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(ntdlib VERSION 4.1.0 LANGUAGES C CXX)

# CMake 3.20 has no PROJECT_IS_TOP_LEVEL. ntdlib is the top-level project when the build starts in its folder.
if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(NTDLIB_TOP_LEVEL ON)
else()
    set(NTDLIB_TOP_LEVEL OFF)
endif()

option(NTDLIB_DISC "Build ntdlib::disc_containers. It requires zlib, Zstandard, liblzma and bzip2 from xsflib."
       ${NTDLIB_TOP_LEVEL})
option(NTDLIB_SEQUENCE_EXPORT "Build ntdlib::sequence_export, the MIDI, SoundFont 2 and SFZ export. It requires seqlib."
       OFF)
option(NTDLIB_TESTS "Build ntdlib's tests and register them with CTest." ${NTDLIB_TOP_LEVEL})
option(NTDLIB_INSTALL "Install ntdlib's headers, libraries and CMake package. Requires installed dependencies." OFF)

if (NTDLIB_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# xsflib. A project that uses ntdlib adds xsflib first. A build of ntdlib alone adds the copy in externals/xsflib, or
# finds an installed xsflib.
if (NOT TARGET xsflib::common)
    if (EXISTS ${PROJECT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt)
        set(XSFLIB_PSF ${NTDLIB_TOP_LEVEL})
        set(XSFLIB_DISC_LIBRARIES ${NTDLIB_DISC})
        set(XSFLIB_INSTALL ${NTDLIB_INSTALL})
        add_subdirectory(externals/xsflib)
    else()
        find_package(xsflib 1.10 REQUIRED)
    endif()
endif()
if (NTDLIB_DISC AND NOT TARGET xsflib::lzma)
    message(FATAL_ERROR "ntdlib::disc_containers requires the libraries that xsflib finds with XSFLIB_DISC_LIBRARIES. "
                        "Set it ON before adding xsflib.")
endif()

# seqlib, for ntdlib::sequence_export. The project that uses ntdlib adds seqlib first. A build of ntdlib alone takes the
# copy that xsflib adds from its submodule, or finds an installed seqlib.
if (NTDLIB_SEQUENCE_EXPORT AND NOT TARGET seqlib::seqlib)
    find_package(seqlib 1.4 REQUIRED)
endif()

if (NTDLIB_TESTS)
    enable_testing()
endif()

# Builds the source files in the arguments to follow the FPU's rounding mode at run time. The compiler then keeps each
# floating-point step as the code writes it, under any rounding mode.
function(ntdlib_follow_rounding_mode)
    if (MSVC)
        set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS /fp:strict)
    else()
        set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS -frounding-math)
    endif()
endfunction()

# The builders of synthetic sound archives and executables for tests, the checks of a conversion's output, and a guest
# that calls a Wii game's functions on the Broadway interpreter, beside xsflib's checks.
add_library(ntdlib_testing INTERFACE)
add_library(ntdlib::testing ALIAS ntdlib_testing)
target_include_directories(ntdlib_testing INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                                                    $<INSTALL_INTERFACE:include>)
target_link_libraries(ntdlib_testing INTERFACE xsflib::testing)
target_compile_features(ntdlib_testing INTERFACE cxx_std_20)
set_target_properties(ntdlib_testing PROPERTIES EXPORT_NAME testing)
set_property(GLOBAL APPEND PROPERTY NTDLIB_INSTALLED_TARGETS ntdlib_testing)

# The common files: the consoles' addresses, the PowerPC's fused operations, the consoles' conversions of a float to
# an integer, AArch64's minimum, maximum and reciprocal, DOL executables, REL modules, the builds of the Wii's sound
# code that a main.dol links, a decoder of DEFLATE and zlib streams, the pieces of music that a game plays as several
# sequences (sound_pieces.h), the length of a held note (held_notes.h), and the shortest decimal text of a float
# (shortest_decimal.h).
# The decoders of Yay0 and Yaz0, two compression formats of Nintendo's games.
# The roundings that several modules share (rounding.h): a value up to a multiple, and a float to a 32-bit integer.
xsflib_add_project_library(ntdlib common src/common/dol.cpp src/common/inflate.cpp src/common/rel_module.cpp
                           src/common/sound_build.cpp src/common/yay0.cpp src/common/yaz0.cpp)
target_link_libraries(ntdlib_common PUBLIC xsflib::common)
xsflib_add_project_test(ntdlib inflate ntdlib::common)
xsflib_add_project_test(ntdlib rel_module ntdlib::common)
xsflib_add_project_test(ntdlib sound_build ntdlib::common)
xsflib_add_project_test(ntdlib aarch64 ntdlib::common)
xsflib_add_project_test(ntdlib fused ntdlib::common)
xsflib_add_project_test(ntdlib shortest_decimal ntdlib::common)
xsflib_add_project_test(ntdlib rounding ntdlib::common)
xsflib_add_project_test(ntdlib dol ntdlib::common)
xsflib_add_project_test(ntdlib sound_pieces ntdlib::common)
xsflib_add_project_test(ntdlib yay0 ntdlib::common)
xsflib_add_project_test(ntdlib yaz0 ntdlib::common)

# The decoders of raw sample data (DSP-ADPCM, IMA-ADPCM, AFC, PCM8 and PCM16), the decoding of a sound archive's wave
# to PCM, the encoders of DSP-ADPCM and IMA-ADPCM, a band-limited interpolator for sample-rate conversion, and
# band-limited steps for the channels of a sound chip.
xsflib_add_project_library(ntdlib wave src/wave/adpcm.cpp src/wave/blep.cpp src/wave/decode.cpp src/wave/encode.cpp
                           src/wave/encode_ima.cpp src/wave/kaiser_sinc.cpp src/wave/pcm.cpp src/wave/sinc.cpp)
target_link_libraries(ntdlib_wave PRIVATE xsflib::common)
xsflib_add_project_test(ntdlib wave ntdlib::wave)
xsflib_add_project_test(ntdlib wave_encode ntdlib::wave)
xsflib_add_project_test(ntdlib wave_encode_ima ntdlib::wave)
xsflib_add_project_test(ntdlib wave_sinc ntdlib::wave)
xsflib_add_project_test(ntdlib wave_blep ntdlib::wave)

# The 3DS's effects on the aux buses, as the ARM11 runs them: the reverb and the delay of nw::snd and nn::snd, and the
# single-precision arithmetic of their setup.
xsflib_add_project_library(ntdlib ctr_fx src/ctr_fx/effects.cpp src/ctr_fx/vfp.cpp)
xsflib_add_project_test(ntdlib ctr_fx ntdlib::ctr_fx)
xsflib_add_project_test(ntdlib ctr_fx_vfp ntdlib::ctr_fx)

# The tracks that known 3DS games play as a stream sound or a sequence starts, by the sound archive's SHA-1 and the
# sound's name. A player that runs no model of the 3DS's library takes them from here.
xsflib_add_project_library(ntdlib ctr_starting_tracks src/ctr/snd/starting_tracks.cpp)
xsflib_add_project_test(ntdlib ctr_starting_tracks ntdlib::ctr_starting_tracks)

# How known Wii U games start some sequences, and the later states that they give them, by the sound archive's SHA-1
# and the sequence's label. A converter that runs no model of a game's code takes them from here.
xsflib_add_project_library(ntdlib cafe_sequence_starts src/cafe/snd/sequence_starts.cpp)
xsflib_add_project_test(ntdlib cafe_sequence_starts ntdlib::cafe_sequence_starts)

# The readers of JAudio2's sound data: the start-up archive (BAA) with its sound table, sequence collection, wave
# systems and banks, and RARC archives. The readers of JAudio1's start-up archive (AAF), of the bank file of Pikmin's
# JAudio (.bx), of the sequence index (BARC) and of JMap tables (BCSV). And the length analysis of a sound in game mode
# (LoopFinder and game_lengths.h).
xsflib_add_project_library(ntdlib jaudio src/jaudio/aaf.cpp src/jaudio/archive.cpp src/jaudio/bank.cpp
                           src/jaudio/barc.cpp src/jaudio/bx.cpp src/jaudio/chord_files.cpp src/jaudio/chord_keys.cpp
                           src/jaudio/chord_layer.cpp src/jaudio/chord_player.cpp src/jaudio/game_lengths.cpp
                           src/jaudio/jac_effects.cpp src/jaudio/jmap.cpp src/jaudio/loop_finder.cpp
                           src/jaudio/pikmin2_pieces.cpp src/jaudio/rarc.cpp src/jaudio/wave_system.cpp
                           src/jaudio/wind_waker_ports.cpp)
target_link_libraries(ntdlib_jaudio PUBLIC xsflib::common ntdlib::common)
xsflib_add_project_test(ntdlib jaudio ntdlib::jaudio)
xsflib_add_project_test(ntdlib aaf ntdlib::jaudio)
xsflib_add_project_test(ntdlib loop_finder ntdlib::jaudio)
xsflib_add_project_test(ntdlib game_lengths ntdlib::jaudio)
xsflib_add_project_test(ntdlib chord_files ntdlib::jaudio)
xsflib_add_project_test(ntdlib chord_keys ntdlib::jaudio)
xsflib_add_project_test(ntdlib chord_player ntdlib::jaudio)
xsflib_add_project_test(ntdlib jaudio_games ntdlib::jaudio)

# The models of JAudio's Switch ports. Each port's `main` links Hagi, an emulator of the GameCube's and the Wii's
# hardware. Hagi runs JAudio's DSP program as a high-level emulation. jaudio2 models that emulation and the sound
# library above it in the Super Mario Galaxy games: JAudio2's arithmetic, oscillators, banks, channels and sequences,
# and the engine that plays a sound of the start-up archive's sound table.
xsflib_add_project_library(ntdlib jaudio2 src/jaudio2/calc.cpp src/jaudio2/channel.cpp src/jaudio2/dsp.cpp
                           src/jaudio2/engine.cpp src/jaudio2/instruments.cpp src/jaudio2/oscillator.cpp
                           src/jaudio2/output.cpp src/jaudio2/track.cpp src/jaudio2/track_commands.cpp)
target_link_libraries(ntdlib_jaudio2 PUBLIC ntdlib::jaudio)

# The model of the older sound library (JAudio1) that Pikmin 2's Switch port builds on the same DSP program: its
# arithmetic, oscillators, banks, channels and sequences, the conductor of the cave music, and the engine that plays
# the game's music and effects on them.
xsflib_add_project_library(ntdlib jaudio1 src/jaudio1/calc.cpp src/jaudio1/channel.cpp src/jaudio1/conductor.cpp
                           src/jaudio1/engine.cpp src/jaudio1/engine_effects.cpp src/jaudio1/sound_files.cpp
                           src/jaudio1/instruments.cpp src/jaudio1/oscillator.cpp src/jaudio1/track.cpp
                           src/jaudio1/track_levels.cpp src/jaudio1/track_registers.cpp src/jaudio1/track_notes.cpp
                           src/jaudio1/track_parser.cpp)
target_link_libraries(ntdlib_jaudio1 PUBLIC ntdlib::jaudio2)
# The parts of the engine and of the tracks build as one translation unit each. The compiler can then inline their
# small helpers across them.
set_target_properties(ntdlib_jaudio1 PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
set_source_files_properties(src/jaudio1/engine.cpp src/jaudio1/engine_effects.cpp src/jaudio1/sound_files.cpp
                            PROPERTIES UNITY_GROUP jaudio1_engine)
set_source_files_properties(src/jaudio1/track.cpp src/jaudio1/track_levels.cpp src/jaudio1/track_registers.cpp
                            src/jaudio1/track_notes.cpp src/jaudio1/track_parser.cpp PROPERTIES UNITY_GROUP jaudio1_track)
xsflib_add_project_test(ntdlib jaudio1 ntdlib::jaudio1)

# The model of the C JAudio (the Jac) of Pikmin's Switch port, on the same DSP program: its arithmetic, oscillators,
# banks with their wave scenes in ARAM, channels and sequences, and the game's code that plays its sounds.
xsflib_add_project_library(ntdlib jac src/jac/bank.cpp src/jac/calc.cpp src/jac/channel.cpp src/jac/channel_dsp.cpp
                           src/jac/channel_lists.cpp src/jac/channel_levels.cpp src/jac/channel_oneshot.cpp
                           src/jac/engine.cpp src/jac/engine_game.cpp src/jac/oscillator.cpp src/jac/track.cpp
                           src/jac/track_registers.cpp src/jac/track_notes.cpp src/jac/track_parser.cpp)
target_link_libraries(ntdlib_jac PUBLIC ntdlib::jaudio1 ntdlib::jaudio2)
# The parts of the channels, of the engine and of the tracks build as one translation unit each. The compiler can then
# inline their small helpers across them.
set_target_properties(ntdlib_jac PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
set_source_files_properties(src/jac/channel.cpp src/jac/channel_dsp.cpp src/jac/channel_lists.cpp
                            src/jac/channel_levels.cpp src/jac/channel_oneshot.cpp PROPERTIES UNITY_GROUP jac_channel)
set_source_files_properties(src/jac/engine.cpp src/jac/engine_game.cpp PROPERTIES UNITY_GROUP jac_engine)
set_source_files_properties(src/jac/track.cpp src/jac/track_registers.cpp src/jac/track_notes.cpp
                            src/jac/track_parser.cpp PROPERTIES UNITY_GROUP jac_track)
xsflib_add_project_test(ntdlib jac ntdlib::jac)

# The readers of NintendoWare's sound archives for the 3DS (CSAR), the Wii U and the Switch (FSAR), with their files and
# the decoding of their waves. The reader and the writer of the stream files (CSTM and FSTM) are here too: the models
# play stream sounds from them. The reader of SARC archives gives the group files that a game keeps in them. The rules
# of a game give what its code does with an archive's sounds: the tracks it turns down and the reverbs on aux bus B.
xsflib_add_project_library(ntdlib sar src/sar/formats.cpp src/sar/decode.cpp src/sar/sarc.cpp src/sar/stream_file.cpp
                           src/sar/game_rules.cpp src/sar/sound_archive.cpp)
target_link_libraries(ntdlib_sar PUBLIC ntdlib::common ntdlib::wave)
xsflib_add_project_test(ntdlib sar ntdlib::sar ntdlib::wave)
xsflib_add_project_test(ntdlib sar_stop_at_first_problem ntdlib::sar)
xsflib_add_project_test(ntdlib sar_set_archive ntdlib::sar)
xsflib_add_project_test(ntdlib length_cases ntdlib::sar)
xsflib_add_project_test(ntdlib sar_game_rules ntdlib::sar)
xsflib_add_project_test(ntdlib sar_checks ntdlib::sar)

# The archive mode's parts of the rip tools for the 3DS, the Wii U and the Switch: the sounds to rip, the data they
# require, the groups outside an archive, the folders of a game's archives, the timing helpers, and the libraries and
# minis.
if (TARGET xsflib::psf)
    xsflib_add_project_library(ntdlib rip_archive src/rip/archive.cpp)
    target_link_libraries(ntdlib_rip_archive PUBLIC ntdlib::sar xsflib::psf xsflib::rip)
    xsflib_add_project_test(ntdlib rip_archive ntdlib::rip_archive)
    xsflib_add_project_test(ntdlib rip_archive_mode ntdlib::rip_archive)
endif()

# The readers of Bezel Engine's data sheets, and the levels that its games give their sounds there.
xsflib_add_project_library(ntdlib bezel src/bezel/sheet.cpp src/bezel/sound_levels.cpp)
target_link_libraries(ntdlib_bezel PUBLIC xsflib::common)
xsflib_add_project_test(ntdlib bezel ntdlib::bezel)

# The readers of NintendoWare for Revolution's sound archives for the Wii (RSAR), with their files and the decoding of
# their waves. The reader and the writer of the stream files (RSTM) are here too, and the copies of an archive that
# keep only the data that some sounds play.
xsflib_add_project_library(ntdlib rsar src/rsar/formats.cpp src/rsar/decode.cpp src/rsar/stream_file.cpp
                           src/rsar/tables.cpp src/rsar/cut.cpp)
target_link_libraries(ntdlib_rsar PUBLIC ntdlib::common ntdlib::wave)
xsflib_add_project_test(ntdlib rsar ntdlib::rsar)
xsflib_add_project_test(ntdlib rsar_cut ntdlib::rsar)

# The DS's cartridge images (NDS ROMs): the header, the ARM9 and ARM7 binaries, their overlays and the file system. The
# writer lays an image out again with new bytes for its files.
xsflib_add_project_library(ntdlib ntr src/ntr/rom.cpp src/ntr/rom_writer.cpp src/ntr/sound.cpp)
target_link_libraries(ntdlib_ntr PUBLIC xsflib::common ntdlib::wave)
xsflib_add_project_test(ntdlib ntr_rom ntdlib::ntr)
xsflib_add_project_test(ntdlib ntr_rom_writer ntdlib::ntr)
xsflib_add_project_test(ntdlib ntr_sound ntdlib::ntr)

# A model of the DS's sound driver on the ARM7 (SND): its sequencer, its channels and its commands. The sequencer reads
# its sequences through the decoder of their bytecode, in the form of ntdlib::commands. The encoder comes with it.
xsflib_add_project_library(ntdlib ntr_snd src/ntr/snd/commands.cpp src/ntr/snd/driver.cpp
                           src/ntr/snd/driver_channels.cpp src/ntr/snd/driver_lengths.cpp src/ntr/snd/driver_tracks.cpp)
target_link_libraries(ntdlib_ntr_snd PUBLIC xsflib::common ntdlib::commands)
xsflib_add_project_test(ntdlib ntr_snd ntdlib::ntr_snd ntdlib::ntr)
xsflib_add_project_test(ntdlib ntr_snd_commands ntdlib::ntr_snd)

# The readers and writers of the DS's sound archives (SDAT) and their files, with the decoding and encoding of their
# waves and the decoding of their streams.
xsflib_add_project_library(ntdlib sdat src/sdat/formats.cpp src/sdat/decode.cpp src/sdat/layout.cpp src/sdat/heap.cpp
    src/sdat/files.cpp)
target_link_libraries(ntdlib_sdat PUBLIC xsflib::common ntdlib::ntr ntdlib::wave)
xsflib_add_project_test(ntdlib sdat ntdlib::sdat)
xsflib_add_project_test(ntdlib sdat_files ntdlib::sdat)
xsflib_add_project_test(ntdlib sar_encodings ntdlib::sdat ntdlib::sar ntdlib::rsar)

# A sound archive cut into pieces for an editor, with its sequences decoded, and joined again.
xsflib_add_project_library(ntdlib sdat_cut src/sdat/cut.cpp)
target_link_libraries(ntdlib_sdat_cut PUBLIC ntdlib::sdat ntdlib::commands PRIVATE ntdlib::ntr_snd)
xsflib_add_project_test(ntdlib sdat_cut ntdlib::sdat_cut)

# The readers and writers of the files of Procyon Studio's sound driver for the DS, Digital Sound Elements (DSE): songs,
# wave banks and banks of sound effects. A model of the driver's part on the ARM9 plays songs.
xsflib_add_project_library(ntdlib dse src/dse/formats.cpp src/dse/driver.cpp src/dse/driver_sequences.cpp
                           src/dse/driver_events.cpp src/dse/driver_voices.cpp src/dse/sets.cpp)
target_link_libraries(ntdlib_dse PUBLIC xsflib::common ntdlib::ntr_snd)
# The sequences and the handlers of their commands build as one translation unit.
set_target_properties(ntdlib_dse PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
set_source_files_properties(src/dse/driver_sequences.cpp src/dse/driver_events.cpp PROPERTIES UNITY_GROUP dse_sequences)
xsflib_add_project_test(ntdlib dse ntdlib::dse)
xsflib_add_project_test(ntdlib dse_driver ntdlib::dse)

# DSE's tracks in the common form of the commands, and a game's DSE files cut into pieces for an editor.
xsflib_add_project_library(ntdlib dse_layout src/dse/commands.cpp src/dse/layout.cpp)
target_link_libraries(ntdlib_dse_layout PUBLIC ntdlib::dse ntdlib::commands)
xsflib_add_project_test(ntdlib dse_layout ntdlib::dse_layout)

# DSE's songs and sound effects on the model of DSE's driver, the model of the DS's sound driver and the sound unit,
# with the steps of archive mode.
xsflib_add_project_library(ntdlib dse_player src/dse/player.cpp src/dse/archive_source.cpp)
target_link_libraries(ntdlib_dse_player PUBLIC ntdlib::dse ntdlib::ntr)
xsflib_add_project_test(ntdlib dse_player ntdlib::dse_player)
xsflib_add_project_test(ntdlib dse_archive_source ntdlib::dse_player)

# The length analysis of DSE's songs and sound effects on the model of DSE's driver, with xsflib's analysis.
xsflib_add_project_library(ntdlib dse_lengths src/dse/length_model.cpp)
target_link_libraries(ntdlib_dse_lengths PUBLIC ntdlib::dse_player xsflib::rip)
xsflib_add_project_test(ntdlib dse_length_model ntdlib::dse_lengths)

# An interpreter of the DS's ARM7TDMI and ARM946E-S: the ARM and Thumb instruction sets, the exceptions, and the
# ARM946E-S's system control coprocessor with its tightly coupled memories.
xsflib_add_project_library(ntdlib arm src/arm/arm.cpp src/arm/cp15.cpp src/arm/cpu.cpp src/arm/thumb.cpp)
target_link_libraries(ntdlib_arm PUBLIC xsflib::common)
xsflib_add_project_test(ntdlib arm ntdlib::arm)
xsflib_add_project_test(ntdlib arm_system ntdlib::arm)
xsflib_add_project_test(ntdlib arm_thumb ntdlib::arm)

# The ARM7's side of a DS: the CPU, the memories, the timers, the interrupts, the IPC and the sound unit, with a BIOS of
# high-level calls. It runs a cartridge's ARM7 binary with its sound driver.
xsflib_add_project_library(ntdlib ntr_arm7 src/ntr/arm7.cpp src/ntr/spi_devices.cpp src/ntr/timers.cpp)
target_link_libraries(ntdlib_ntr_arm7 PUBLIC ntdlib::ntr ntdlib::arm)
xsflib_add_project_test(ntdlib ntr_arm7 ntdlib::ntr_arm7)

# A DS with both CPUs: the ARM9's side beside the ARM7's, with the card's reads from the image. It boots a cartridge
# image and runs the game's code.
xsflib_add_project_library(ntdlib ntr_machine src/ntr/machine.cpp src/ntr/machine_card.cpp src/ntr/machine_io.cpp)
target_link_libraries(ntdlib_ntr_machine PUBLIC ntdlib::ntr_arm7)
# The parts of the machine build as one translation unit (machine_internal.h).
set_target_properties(ntdlib_ntr_machine PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
set_source_files_properties(src/ntr/machine.cpp src/ntr/machine_card.cpp src/ntr/machine_io.cpp
                            PROPERTIES UNITY_GROUP ntr_machine)
xsflib_add_project_test(ntdlib ntr_machine ntdlib::ntr_machine)

# Plays sounds on a cartridge's sound driver on the ARM7's side of a DS. The player takes the ARM9's part.
xsflib_add_project_library(ntdlib ntr_driver_player src/ntr/driver_player.cpp)
target_link_libraries(ntdlib_ntr_driver_player PUBLIC ntdlib::ntr_arm7 ntdlib::ntr_snd)

# Plays a sound archive's sequences on the model of the DS's sound driver, with the ARM9's part on the host: the loading
# of the files and the commands of the SDK's sound archive player, and snapshots.
xsflib_add_project_library(ntdlib ntr_archive_player src/ntr/archive_player.cpp)
target_link_libraries(ntdlib_ntr_archive_player PUBLIC ntdlib::ntr ntdlib::ntr_snd ntdlib::sdat)
xsflib_add_project_test(ntdlib ntr_archive_player ntdlib::ntr_archive_player)
xsflib_add_project_test(ntdlib ntr_archive_player_play ntdlib::ntr_archive_player)
xsflib_add_project_test(ntdlib ntr_archive_source ntdlib::ntr_archive_player)

# What known DS games' code sets as it starts a sequence of their sound archives (the archive player's settings and
# commands), and the states of a sequence that the code keeps set while it plays.
xsflib_add_project_library(ntdlib ntr_profiles src/ntr/game_profiles.cpp)
target_link_libraries(ntdlib_ntr_profiles PUBLIC ntdlib::ntr_archive_player)
xsflib_add_project_test(ntdlib ntr_game_profiles ntdlib::ntr_profiles)

# The length analysis of a sound archive's sequence on the model of the DS's sound driver, with xsflib's analysis.
xsflib_add_project_library(ntdlib ntr_lengths src/ntr/length_model.cpp)
target_link_libraries(ntdlib_ntr_lengths PUBLIC ntdlib::ntr_archive_player xsflib::rip)
xsflib_add_project_test(ntdlib ntr_length_model ntdlib::ntr_lengths)
xsflib_add_project_test(ntdlib ntr_length_cases ntdlib::ntr_lengths ntdlib::nwsnd_commands)

# The archive mode's parts of a rip tool for the DS's sound archives: the sequences to rip, what they require, the part
# of an archive that a library holds, and the libraries and minis.
if (TARGET xsflib::psf)
    xsflib_add_project_library(ntdlib ntr_rip src/ntr/rip.cpp)
    target_link_libraries(ntdlib_ntr_rip PUBLIC ntdlib::ntr_lengths xsflib::psf)
    xsflib_add_project_test(ntdlib ntr_rip ntdlib::ntr_rip)

    # The same parts for DSE's songs and sound effects.
    xsflib_add_project_library(ntdlib dse_rip src/dse/rip.cpp)
    target_link_libraries(ntdlib_dse_rip PUBLIC ntdlib::dse xsflib::psf xsflib::rip)
    xsflib_add_project_test(ntdlib dse_rip ntdlib::dse_rip ntdlib::dse_lengths)
endif()

# The readers of Factor 5's MusyX data for the GameCube: a project's groups, a pool's macros, tables, keymaps and layers,
# a sample directory with the decoding of its samples, and songs. Also the file of Hudson's sound manager over MusyX, and
# the sound test of Mario Party 4.
xsflib_add_project_library(ntdlib musyx src/musyx/msm.cpp src/musyx/pool.cpp src/musyx/project.cpp
                           src/musyx/samples.cpp src/musyx/song.cpp)
target_link_libraries(ntdlib_musyx PUBLIC xsflib::common PRIVATE ntdlib::wave)
xsflib_add_project_test(ntdlib musyx ntdlib::musyx)
xsflib_add_project_test(ntdlib musyx_msm ntdlib::musyx)

# The conversion of Nintendo's sound data to seqlib's MIDI, SoundFont 2 and SFZ files: NintendoWare's sound archives
# (sequence_export), JAudio's sound data (sequence_export_jaudio, in src/sequence_export/jaudio), the DS's sound
# archives (sequence_export_ntr, in src/sequence_export/ntr), DSE's songs and sound effects (sequence_export_dse, in
# src/sequence_export/dse). A program that converts only NintendoWare's archives links none of JAudio's readers, the disc
# images and the DS's driver. A program that converts the DS's archives also converts the songs and sound effects
# of DSE in cartridge images. sequence_export_ntr_profiles turns the profiles of known DS games (ntr_profiles) into
# the options and arrangements of such a program.
if (NTDLIB_SEQUENCE_EXPORT)
    xsflib_add_project_library(ntdlib sequence_export
        src/sequence_export/archive_converter.cpp
        src/sequence_export/arrangements.cpp
        src/sequence_export/convert.cpp
        src/sequence_export/convert_rsar.cpp
        src/sequence_export/envelope.cpp
        src/sequence_export/filter.cpp
        src/sequence_export/filter_tables_cafe.cpp
        src/sequence_export/filter_tables_ctr.cpp
        src/sequence_export/filter_tables_revolution.cpp
        src/sequence_export/filter_tables_switch.cpp
        src/sequence_export/frame.cpp
        src/sequence_export/instruments.cpp
        src/sequence_export/midi_recorder.cpp
        src/sequence_export/passes.cpp
        src/sequence_export/perform_pieces.cpp
        src/sequence_export/perform_sequence.cpp
        src/sequence_export/performance.cpp
        src/sequence_export/performer.cpp
        src/sequence_export/resampling.cpp
        src/sequence_export/rsar_instruments.cpp
        src/sequence_export/sf2_build.cpp
        src/sequence_export/sfz.cpp
        src/sequence_export/sfz_rsar.cpp
        src/sequence_export/sfz_track.cpp
        src/sequence_export/zone_low_pass.cpp)
    target_link_libraries(ntdlib_sequence_export PUBLIC seqlib::seqlib ntdlib::ax ntdlib::common ntdlib::nwsnd
                                                        ntdlib::sar ntdlib::rsar ntdlib::wave xsflib::console)
    xsflib_add_project_library(ntdlib sequence_export_jaudio
        src/sequence_export/jaudio/chord_companion.cpp
        src/sequence_export/jaudio/conductor.cpp
        src/sequence_export/jaudio/convert.cpp
        src/sequence_export/jaudio/envelope.cpp
        src/sequence_export/jaudio/game.cpp
        src/sequence_export/jaudio/game_input.cpp
        src/sequence_export/jaudio/instruments.cpp
        src/sequence_export/jaudio/performance.cpp
        src/sequence_export/jaudio/performer_channels.cpp
        src/sequence_export/jaudio/performer_commands.cpp
        src/sequence_export/jaudio/performer_companion.cpp
        src/sequence_export/jaudio/performer_conductor.cpp
        src/sequence_export/jaudio/performer_jaudio1.cpp
        src/sequence_export/jaudio/performer_recording.cpp
        src/sequence_export/jaudio/performer_tracks.cpp
        src/sequence_export/jaudio/sfz.cpp
        src/sequence_export/jaudio/track_filter.cpp
        src/sequence_export/jaudio/wave.cpp)
    target_link_libraries(ntdlib_sequence_export_jaudio PUBLIC ntdlib::sequence_export ntdlib::disc ntdlib::jaudio)
    xsflib_add_project_library(ntdlib sequence_export_dse
        src/sequence_export/dse/convert.cpp
        src/sequence_export/dse/instruments.cpp
        src/sequence_export/dse/performance.cpp
        src/sequence_export/dse/sfz.cpp
        src/sequence_export/dse/sequence_input.cpp)
    target_link_libraries(ntdlib_sequence_export_dse PUBLIC ntdlib::sequence_export ntdlib::dse_player ntdlib::sdat)
    xsflib_add_project_library(ntdlib sequence_export_ntr
        src/sequence_export/ntr/archive_input.cpp
        src/sequence_export/ntr/convert.cpp
        src/sequence_export/ntr/instruments.cpp
        src/sequence_export/ntr/performance.cpp
        src/sequence_export/ntr/sfz.cpp)
    target_link_libraries(ntdlib_sequence_export_ntr PUBLIC ntdlib::sequence_export ntdlib::ntr_archive_player
                                                            ntdlib::sequence_export_dse)
    xsflib_add_project_library(ntdlib sequence_export_ntr_profiles src/sequence_export/ntr/game_profiles.cpp)
    target_link_libraries(ntdlib_sequence_export_ntr_profiles PUBLIC ntdlib::sequence_export_ntr ntdlib::ntr_profiles)
    xsflib_add_project_test(ntdlib sequence_export_performance ntdlib::sequence_export)
    xsflib_add_project_test(ntdlib sequence_export_frame ntdlib::sequence_export)
    xsflib_add_project_test(ntdlib sequence_export_arrangements ntdlib::sequence_export)
    xsflib_add_project_test(ntdlib sequence_export ntdlib::sequence_export)
    xsflib_add_project_test(ntdlib sequence_export_revolution ntdlib::sequence_export)
    xsflib_add_project_test(ntdlib sequence_export_jaudio ntdlib::sequence_export_jaudio)
    xsflib_add_project_test(ntdlib sequence_export_ntr ntdlib::sequence_export_ntr)
    xsflib_add_project_test(ntdlib sequence_export_ntr_editor ntdlib::sequence_export_ntr)
    xsflib_add_project_test(ntdlib sequence_export_ntr_profiles ntdlib::sequence_export_ntr_profiles)
    xsflib_add_project_test(ntdlib sequence_export_dse ntdlib::sequence_export_dse)
    xsflib_add_project_test(ntdlib sequence_export_dse_editor ntdlib::sequence_export_dse)
    xsflib_add_project_test(ntdlib sequence_export_switch ntdlib::sequence_export)
    xsflib_add_project_test(ntdlib sequence_export_ctr ntdlib::sequence_export)
    xsflib_add_project_test(ntdlib sequence_export_resampling ntdlib::sequence_export)
    xsflib_add_project_test(ntdlib sequence_export_zone_low_pass ntdlib::sequence_export)
    if (NTDLIB_TESTS)
        # These tests read private headers of sequence_export: the Wii's wii_pan.h, and the zones' low-passes in
        # sf2_build.h and zone_low_pass.h.
        foreach (test IN ITEMS revolution resampling zone_low_pass)
            target_include_directories(ntdlib_sequence_export_${test}_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
        endforeach()
    endif()
endif()

# The PowerPC interpreter of the GameCube's Gekko, the Wii's Broadway and the Wii U's Espresso, for game mode. Its
# floating point follows FPSCR's rounding mode at run time.
xsflib_add_project_library(ntdlib ppc src/ppc/cpu.cpp src/ppc/fpu.cpp)
target_link_libraries(ntdlib_ppc PUBLIC ntdlib::common)
ntdlib_follow_rounding_mode(src/ppc/fpu.cpp)

# The DSP of the GameCube, the Wii and the Wii U, with each console's memory. Its sources build as one translation unit.
# The compiler can then inline the small register and flag helpers into the instructions. instantiations.cpp comes last.
# That file instantiates the DSP for each console's memory once every member's definition is visible.
xsflib_add_project_library(ntdlib dsp src/dsp/boot_rom.cpp src/dsp/dsp.cpp src/dsp/hardware.cpp src/dsp/instructions.cpp
                           src/dsp/instantiations.cpp)
target_link_libraries(ntdlib_dsp PUBLIC ntdlib::common ntdlib::wave)
set_target_properties(ntdlib_dsp PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 0)
# MSVC: without /bigobj, the instances of the instruction handlers in one object can exceed COFF's limit of sections.
if (MSVC)
    target_compile_options(ntdlib_dsp PRIVATE /bigobj)
endif()
xsflib_add_project_test(ntdlib dsp ntdlib::dsp)
xsflib_add_project_test(ntdlib dsp_accelerator ntdlib::dsp)

# Disc images of the GameCube and the Wii, and a game's files on them: plain images (ISO, NKit), WBFS, WDF, extracted
# discs and titles, the sound archives inside CPK archives, and FSYS archives. It requires no other library.
xsflib_add_project_library(ntdlib disc src/disc/cpk.cpp src/disc/cpk_files.cpp src/disc/cx.cpp src/disc/encodings.cpp
                           src/disc/file_table.cpp src/disc/fsys.cpp src/disc/game.cpp src/disc/image.cpp
                           src/disc/source.cpp src/disc/title.cpp)
target_link_libraries(ntdlib_disc PUBLIC ntdlib::common)
xsflib_add_project_test(ntdlib disc_cpk ntdlib::disc)
xsflib_add_project_test(ntdlib disc_fsys ntdlib::disc)
xsflib_add_project_test(ntdlib disc_options ntdlib::disc)
xsflib_add_project_test(ntdlib disc_title ntdlib::disc)
xsflib_add_project_test(ntdlib disc_text ntdlib::disc)

# The guest that calls a Wii game's functions on the Broadway interpreter, for tests (ntdlib/testing/guest.h).
xsflib_add_project_test(ntdlib guest ntdlib::ppc ntdlib::dsp ntdlib::disc)

# The containers that store a disc image compressed or in sparse blocks: GCZ, CISO, WIA and RVZ. They require zlib,
# Zstandard, liblzma and bzip2.
if (NTDLIB_DISC)
    xsflib_add_project_library(ntdlib disc_containers src/disc/compression.cpp src/disc/containers.cpp
                               src/disc/junk.cpp)
    target_link_libraries(ntdlib_disc_containers PUBLIC ntdlib::disc PRIVATE xsflib::zlib xsflib::zstd xsflib::lzma
                                                                               xsflib::bzip2)
    xsflib_add_project_test(ntdlib disc_gamecube ntdlib::disc_containers xsflib::zlib xsflib::zstd xsflib::lzma
                            xsflib::bzip2)
    xsflib_add_project_test(ntdlib disc_wii ntdlib::disc_containers xsflib::zlib xsflib::zstd xsflib::lzma
                            xsflib::bzip2)

    # The catalogues of the MusyX games whose code keeps its sounds in layouts of its own: the file sets, the songs and
    # the sounds, read from a game's files and code. The readers unpack Retro Studios' packs and Yay0 data and read
    # FSYS archives and modules of code.
    xsflib_add_project_library(ntdlib musyx_catalogue src/musyx/catalogue.cpp src/musyx/catalogue_games.cpp
                               src/musyx/catalogue_metroid.cpp src/musyx/catalogue_pokemon.cpp
                               src/musyx/catalogue_puzzle.cpp src/musyx/catalogue_resident_evil.cpp
                               src/musyx/catalogue_rogue.cpp)
    target_link_libraries(ntdlib_musyx_catalogue PUBLIC ntdlib::musyx ntdlib::disc PRIVATE ntdlib::disc_containers
                                                                                           ntdlib::common)
    xsflib_add_project_test(ntdlib musyx_catalogue ntdlib::musyx_catalogue)
endif()

# The devices that the GameCube's Flipper and the Wii's Hollywood share, for game mode: the DSP interface with the ARAM
# and audio DMAs, the audio interface, the processor interface, the external interface with the clock and SRAM, and a
# disc made of files. MachineBase holds the parts of a machine that don't depend on its other devices. The game-mode
# source holds what game mode does with a set around a machine. The library also holds the host's side of the Z2
# start-up's parameters (drivers/jaudio/z2.h).
xsflib_add_project_library(ntdlib flipper src/flipper/audio_interface.cpp src/flipper/dsp_interface.cpp
                           src/flipper/exi.cpp src/flipper/file_disc.cpp src/flipper/game_source.cpp
                           src/flipper/wave_list.cpp src/flipper/z2_params.cpp)
target_link_libraries(ntdlib_flipper PUBLIC ntdlib::dsp ntdlib::ppc)
xsflib_add_project_test(ntdlib flipper ntdlib::flipper)
xsflib_add_project_test(ntdlib z2_params ntdlib::flipper)

# What the Wii's and the Wii U's AX libraries share: the constants, the voice allocation and the low-pass coefficients.
xsflib_add_project_library(ntdlib ax src/ax/lpf.cpp src/ax/voices.cpp)
target_link_libraries(ntdlib_ax PUBLIC xsflib::common)
xsflib_add_project_test(ntdlib ax ntdlib::ax)

# A common form for the commands of the consoles' sequences, and the units of their values. Each console's decoder and
# encoder use it.
xsflib_add_project_library(ntdlib commands src/commands/sequence.cpp src/commands/units.cpp)

# The model of NintendoWare's sound libraries (nw::snd in NintendoWare for Cafe, nn::atk on the Switch, the 3DS's
# nw::snd and the Wii's NW4R SND): their tables, each console's arithmetic, the envelope and the LFO, and the sequence
# player as a template on a policy. The player reads its commands through the decoder of the bytecode, in the form of
# ntdlib::commands. The encoder and the units of each console's values come with the decoder.
xsflib_add_project_library(ntdlib nwsnd src/nwsnd/commands.cpp src/nwsnd/envelope.cpp src/nwsnd/units.cpp
                           src/nwsnd/util.cpp)
target_link_libraries(ntdlib_nwsnd PUBLIC ntdlib::commands ntdlib::common)
xsflib_add_project_test(ntdlib nwsnd ntdlib::nwsnd)
xsflib_add_project_test(ntdlib nwsnd_sequence ntdlib::nwsnd)
xsflib_add_project_test(ntdlib nwsnd_voice ntdlib::nwsnd)
xsflib_add_project_test(ntdlib nwsnd_channel ntdlib::nwsnd)
xsflib_add_project_test(ntdlib nwsnd_ctr ntdlib::nwsnd)
xsflib_add_project_test(ntdlib nwsnd_revolution ntdlib::nwsnd)

# The decoder and the encoder of the bytecode of NintendoWare's sequences, in the form of ntdlib::commands, and the
# units of each console's values. ntdlib::nwsnd holds them, and the target names them for the programs that link it.
add_library(ntdlib_nwsnd_commands INTERFACE)
add_library(ntdlib::nwsnd_commands ALIAS ntdlib_nwsnd_commands)
target_link_libraries(ntdlib_nwsnd_commands INTERFACE ntdlib::nwsnd)
set_target_properties(ntdlib_nwsnd_commands PROPERTIES EXPORT_NAME nwsnd_commands)
set_property(GLOBAL APPEND PROPERTY NTDLIB_INSTALLED_TARGETS ntdlib_nwsnd_commands)
xsflib_add_project_test(ntdlib nwsnd_commands ntdlib::nwsnd_commands)
xsflib_add_project_test(ntdlib nwsnd_units ntdlib::nwsnd_commands)

# The banks, wave archives and waves that the model's engine loads for its notes from a sound archive of the Wii U or
# the Switch (ntdlib/nwsnd/archive_files.h), read with ntdlib::sar. It's a target of its own: the Wii's model uses
# ntdlib::nwsnd without ntdlib::sar.
add_library(ntdlib_nwsnd_archive INTERFACE)
add_library(ntdlib::nwsnd_archive ALIAS ntdlib_nwsnd_archive)
target_include_directories(ntdlib_nwsnd_archive INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                                                          $<INSTALL_INTERFACE:include>)
target_link_libraries(ntdlib_nwsnd_archive INTERFACE ntdlib::nwsnd ntdlib::sar)
target_compile_features(ntdlib_nwsnd_archive INTERFACE cxx_std_20)
set_target_properties(ntdlib_nwsnd_archive PROPERTIES EXPORT_NAME nwsnd_archive)
set_property(GLOBAL APPEND PROPERTY NTDLIB_INSTALLED_TARGETS ntdlib_nwsnd_archive)
xsflib_add_project_test(ntdlib nwsnd_archive_files ntdlib::nwsnd_archive)

# A sound archive of the 3DS, the Wii U or the Switch cut into pieces for an editor, and joined again: its sequence
# files, decoded into commands, and the runs of bytes between them. The group files beside an archive are cut with it.
# Also the readers and the writers of the sequence files (CSEQ and FSEQ), the wave files (CWAV and FWAV), the wave
# archives (CWAR and FWAR), the groups' files (FGRP) and the archives' STRG and INFO blocks. WithTables puts new STRG
# and INFO blocks in an archive. The banks, wave sound files, waves, wave archives and streams of the Wii U become the
# Switch's, and the Switch's the Wii U's. WithInstruments copies instruments from one bank into another. BankFile
# reads a bank as the values of its instruments and regions, and lays a bank out from them as NintendoWare's tools do.
# WithWaveSoundValues and WithWaveSoundWave edit a wave sound file's values and waves. ReadArchiveModel reads an archive of the Wii U or the Switch as the fields of its items, and BuildArchive lays an
# archive out from them as NintendoWare's tools do.
xsflib_add_project_library(ntdlib sar_layout src/sar/archive_model.cpp src/sar/bank_edit.cpp src/sar/bank_file.cpp
                           src/sar/byte_order.cpp src/sar/groups.cpp src/sar/info_block.cpp src/sar/layout.cpp
                           src/sar/layout_core.cpp src/sar/sequence_file.cpp src/sar/stream_details.cpp
                           src/sar/string_block.cpp src/sar/tables.cpp src/sar/wave_file.cpp src/sar/wave_sound_edit.cpp)
target_link_libraries(ntdlib_sar_layout PUBLIC ntdlib::nwsnd_commands ntdlib::sar)
xsflib_add_project_test(ntdlib sar_archive_model ntdlib::sar_layout)
xsflib_add_project_test(ntdlib sar_bank_edit ntdlib::sar_layout)
xsflib_add_project_test(ntdlib sar_bank_file ntdlib::sar_layout)
xsflib_add_project_test(ntdlib sar_byte_order ntdlib::sar_layout)
xsflib_add_project_test(ntdlib sar_groups ntdlib::sar_layout)
xsflib_add_project_test(ntdlib sar_info_block ntdlib::sar_layout)
xsflib_add_project_test(ntdlib sar_layout ntdlib::sar_layout)
xsflib_add_project_test(ntdlib sar_sequence_file ntdlib::sar_layout)
xsflib_add_project_test(ntdlib sar_stream_file ntdlib::sar_layout)
xsflib_add_project_test(ntdlib sar_string_block ntdlib::sar_layout)
xsflib_add_project_test(ntdlib sar_tables ntdlib::sar_layout)
xsflib_add_project_test(ntdlib sar_wave_file ntdlib::sar_layout)
xsflib_add_project_test(ntdlib sar_wave_sound_edit ntdlib::sar_layout)

# A sound archive of the Wii (RSAR) cut into pieces for an editor, and joined again, by the rules of sar_layout: its
# sequence files (RSEQ), decoded into commands, and the runs of bytes between them. Also the reader and the writer of
# the sequence files, the Wii's banks and waves as files of the 3DS, the Wii U or the Switch, the models of the Wii's
# banks, waves, wave archives and wave sound files, and the model of an archive's items. An archive and these files are laid out from their
# models as NintendoWare for Revolution's tools lay them out.
xsflib_add_project_library(ntdlib rsar_layout src/rsar/archive_model.cpp src/rsar/convert.cpp src/rsar/layout.cpp
                          src/rsar/bank_file.cpp src/rsar/sequence_file.cpp src/rsar/wave_file.cpp
                          src/rsar/wsd_file.cpp)
target_link_libraries(ntdlib_rsar_layout PUBLIC ntdlib::rsar ntdlib::sar_layout)
xsflib_add_project_test(ntdlib rsar_archive_model ntdlib::rsar_layout)
xsflib_add_project_test(ntdlib rsar_bank_file ntdlib::rsar_layout)
xsflib_add_project_test(ntdlib rsar_layout ntdlib::rsar_layout)
xsflib_add_project_test(ntdlib rsar_convert ntdlib::rsar_layout)
xsflib_add_project_test(ntdlib rsar_stream_file ntdlib::rsar_layout)
xsflib_add_project_test(ntdlib rsar_wave_file ntdlib::rsar_layout)
xsflib_add_project_test(ntdlib rsar_wsd_file ntdlib::rsar_layout)

# The Switch's audio renderer (nn::audio) with its reverb, I3DL2 reverb, delay and light limiter, and archive mode's
# model of nn::atk on it: an engine that plays a sound archive's sequences, wave sounds and stream sounds as the Switch
# does, without the game's code. Also the system's downmix of 6 channels to 2, and the reader of the Switch's
# executables (NSO), with the decoder of LZ4's blocks.
xsflib_add_project_library(ntdlib nx src/nx/audio/delay.cpp src/nx/audio/downmix.cpp src/nx/audio/dsp.cpp
                           src/nx/audio/i3dl2.cpp src/nx/audio/light_limiter.cpp src/nx/audio/renderer.cpp
                           src/nx/audio/renderer_effects.cpp src/nx/audio/renderer_frame.cpp src/nx/audio/reverb.cpp
                           src/nx/audio/voice.cpp src/nx/atk/effect.cpp src/nx/atk/engine.cpp
                           src/nx/atk/low_level_voice.cpp src/nx/atk/sequence.cpp src/nx/atk/sound_players.cpp
                           src/nx/atk/voice_renderer.cpp src/nx/lz4.cpp src/nx/nso.cpp)
target_link_libraries(ntdlib_nx PUBLIC ntdlib::nwsnd_archive PRIVATE ntdlib::common ntdlib::wave)
xsflib_add_project_test(ntdlib nx_audio ntdlib::nx)
xsflib_add_project_test(ntdlib nx_audio_effects ntdlib::nx)
xsflib_add_project_test(ntdlib nx_audio_enhanced ntdlib::nx)
xsflib_add_project_test(ntdlib nx_audio_without_mixing ntdlib::nx)
xsflib_add_project_test(ntdlib nx_atk ntdlib::nx)
xsflib_add_project_test(ntdlib nx_atk_engine ntdlib::nx)
xsflib_add_project_test(ntdlib nx_nso ntdlib::nx)

# Bezel Engine's music sync on the Switch: the reader of a stream's regions, markers and jump setting, and the model
# that follows the stream's beats and chords for the sequences that read them through user procedures. The models of
# two games' other user procedures: random draws, counters and a table. The pieces of two games' rhythm modes, and the
# values that the games give them.
xsflib_add_project_library(ntdlib nx_bex src/nx/bex/music_sync.cpp src/nx/bex/rhythm_modes.cpp
                           src/nx/bex/user_procedures.cpp)
target_link_libraries(ntdlib_nx_bex PRIVATE ntdlib::common)
xsflib_add_project_test(ntdlib nx_bex_music_sync ntdlib::nx_bex)
xsflib_add_project_test(ntdlib nx_bex_rhythm_modes ntdlib::nx_bex)
xsflib_add_project_test(ntdlib nx_bex_user_procedures ntdlib::nx_bex)

# The Wii's effects for AX's aux buses (AXFX), from the RVL SDK, and the Photo Channel 1.1's mood effect. Each model
# follows the games' code operation for operation, in single-precision floats where the code uses them.
xsflib_add_project_library(ntdlib rvl_axfx src/rvl/axfx/chorus.cpp src/rvl/axfx/delay.cpp src/rvl/axfx/photo_mood.cpp
                           src/rvl/axfx/reverb_hi.cpp src/rvl/axfx/reverb_std.cpp)
target_link_libraries(ntdlib_rvl_axfx PRIVATE ntdlib::common)
xsflib_add_project_test(ntdlib rvl_axfx ntdlib::rvl_axfx)

# The CPU side of the Wii's AX, from the RVL SDK: the voices and their priorities, the copy of the voices' parameter
# blocks into main memory each frame, the depop block, the command list that controls what the microcode mixes, and
# the mail exchange with the microcode on the DSP. A model of the microcode of December 2006 runs a frame's command
# list without the DSP.
xsflib_add_project_library(ntdlib rvl_ax src/rvl/ax/dsp_link.cpp src/rvl/ax/pb.cpp src/rvl/ax/render.cpp
                           src/rvl/ax/system.cpp src/rvl/ax/voice.cpp)
target_link_libraries(ntdlib_rvl_ax PUBLIC ntdlib::ax ntdlib::dsp ntdlib::rvl_axfx PRIVATE ntdlib::common
                                                                                         ntdlib::flipper)
xsflib_add_project_test(ntdlib rvl_ax ntdlib::rvl_ax)
xsflib_add_project_test(ntdlib rvl_render ntdlib::rvl_ax)

# NW4R SND's engine on the Wii's AX: archive mode's model. It plays a sound archive's sequences, wave sounds and stream
# sounds as the Wii does.
xsflib_add_project_library(ntdlib rvl_snd src/rvl/snd/ax_voice.cpp src/rvl/snd/driver.cpp src/rvl/snd/effects.cpp
                           src/rvl/snd/engine.cpp src/rvl/snd/sequence.cpp src/rvl/snd/stream_sound.cpp
                           src/rvl/snd/wave_sound.cpp)
target_link_libraries(ntdlib_rvl_snd PUBLIC ntdlib::rvl_ax ntdlib::rvl_axfx ntdlib::nwsnd ntdlib::rsar PRIVATE
                                                                                                   ntdlib::common)
xsflib_add_project_test(ntdlib rvl_snd ntdlib::rvl_snd)
xsflib_add_project_test(ntdlib rvl_game_setups ntdlib::rvl_snd)
xsflib_add_project_test(ntdlib rvl_sequence ntdlib::rvl_snd)
xsflib_add_project_test(ntdlib rvl_stream_sound ntdlib::rvl_snd ntdlib::rsar_layout)

# Skyward Sword HD's layer in place of AX: NW4R SND's AX voices on the Switch's renderer, and archive mode's model of
# the game on it.
xsflib_add_project_library(ntdlib rvl_nx src/rvl/nx/archive_model.cpp src/rvl/nx/effects.cpp src/rvl/nx/layer.cpp)
target_link_libraries(ntdlib_rvl_nx PUBLIC ntdlib::rvl_snd ntdlib::nx PRIVATE ntdlib::common)
xsflib_add_project_test(ntdlib rvl_nx ntdlib::rvl_nx)

# The Wii U's AX, the system library's effects for AX's aux buses (AXFX), Nintendo Land's effect classes, and archive
# mode's model of nw::snd on them: an engine that plays a sound archive's sequences, wave sounds and stream sounds as
# the Wii U does, without the game's code. The renderer follows the DSP microcode of the system's sound library.
xsflib_add_project_library(ntdlib cafe src/cafe/ax/render.cpp src/cafe/ax/resample.cpp src/cafe/ax/system.cpp
                           src/cafe/ax/system_output.cpp src/cafe/axfx/chorus.cpp src/cafe/axfx/delay.cpp
                           src/cafe/axfx/math.cpp src/cafe/axfx/reverb_hi.cpp src/cafe/axfx/reverb_std.cpp
                           src/cafe/snd/ax_voice.cpp src/cafe/snd/effects.cpp src/cafe/snd/engine.cpp
                           src/cafe/snd/nintendo_land_effects.cpp src/cafe/snd/renderer.cpp src/cafe/snd/sequence.cpp
                           src/cafe/snd/sound_players.cpp)
target_link_libraries(ntdlib_cafe PUBLIC ntdlib::ax ntdlib::dsp ntdlib::nwsnd_archive PRIVATE ntdlib::common
                                                                                              ntdlib::wave xsflib::rip)
xsflib_add_project_test(ntdlib cafe_ax ntdlib::cafe)
xsflib_add_project_test(ntdlib cafe_ax_enhanced ntdlib::cafe)
xsflib_add_project_test(ntdlib cafe_ax_without_mixing ntdlib::cafe)
xsflib_add_project_test(ntdlib cafe_snd ntdlib::cafe xsflib::rip)
xsflib_add_project_test(ntdlib cafe_nintendo_land_effects ntdlib::cafe)
# An archive model holds the AX system by value, and a snapshot of one is nearly as large. The compiler inlines the
# tests into main. main's frame then takes most of a megabyte. A Windows program's main thread gets 1 MB of stack,
# and the Mac gives it 8 MB. The test gets 8 MB on Windows too.
if (MSVC AND TARGET ntdlib_cafe_snd_test)
    target_link_options(ntdlib_cafe_snd_test PRIVATE "LINKER:/STACK:8388608")
endif()

# The 3DS's DSP in Teakra and archive mode's model on it (cmake/NtdlibCtr.cmake).
include(cmake/NtdlibCtr.cmake)

# The length analysis of a sequence sound of the Wii, the 3DS, the Wii U or the Switch, on the model of the console's
# sound library, with xsflib's analysis (ntdlib/nwsnd/length_model.h). A program links ntdlib::nx for the Switch's part
# (ntdlib/nx/atk/length_model.h), ntdlib::cafe for the Wii U's (ntdlib/cafe/snd/length_model.h), ntdlib::ctr for the
# 3DS's (ntdlib/ctr/snd/length_model.h), or ntdlib::rvl_snd for the Wii's (ntdlib/rvl/snd/length_model.h).
add_library(ntdlib_nwsnd_lengths INTERFACE)
add_library(ntdlib::nwsnd_lengths ALIAS ntdlib_nwsnd_lengths)
target_include_directories(ntdlib_nwsnd_lengths INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                                                          $<INSTALL_INTERFACE:include>)
target_link_libraries(ntdlib_nwsnd_lengths INTERFACE ntdlib::nwsnd xsflib::rip)
target_compile_features(ntdlib_nwsnd_lengths INTERFACE cxx_std_20)
set_target_properties(ntdlib_nwsnd_lengths PROPERTIES EXPORT_NAME nwsnd_lengths)
set_property(GLOBAL APPEND PROPERTY NTDLIB_INSTALLED_TARGETS ntdlib_nwsnd_lengths)
xsflib_add_project_test(ntdlib nwsnd_length_model ntdlib::nwsnd_lengths ntdlib::nx ntdlib::cafe)
xsflib_add_project_test(ntdlib rvl_length_model ntdlib::nwsnd_lengths ntdlib::rvl_snd)
xsflib_add_project_test(ntdlib rvl_nx_length_model ntdlib::nwsnd_lengths ntdlib::rvl_nx)
if (TARGET ntdlib_ctr)
    xsflib_add_project_test(ntdlib ctr_length_model ntdlib::nwsnd_lengths ntdlib::ctr)
endif()

# The archive play test's main, for a test that plays a sound archive's sounds on a model of the sound library
# (ntdlib/testing/archive_play.h).
xsflib_add_project_test(ntdlib archive_play ntdlib::nwsnd)

# The tests that take files supplied on the machine, from a local_tests.cmake in a folder of the source tree, if any.
if (NTDLIB_TESTS)
    xsflib_include_local_tests()
endif()

if (NTDLIB_INSTALL)
    include(cmake/NtdlibInstall.cmake)
endif()
