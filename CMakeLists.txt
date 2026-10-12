# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(ntdlib VERSION 1.0.0 LANGUAGES C CXX)

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
        find_package(xsflib 1.0 REQUIRED)
    endif()
endif()
if (NTDLIB_DISC AND NOT TARGET xsflib::lzma)
    message(FATAL_ERROR "ntdlib::disc_containers requires the libraries that xsflib finds with XSFLIB_DISC_LIBRARIES. "
                        "Set it ON before adding xsflib.")
endif()

# seqlib, for ntdlib::sequence_export. The project that uses ntdlib adds seqlib first. A build of ntdlib alone takes the
# copy that xsflib adds from its submodule, or finds an installed seqlib.
if (NTDLIB_SEQUENCE_EXPORT AND NOT TARGET seqlib::seqlib)
    find_package(seqlib 1.0 REQUIRED)
endif()

if (NTDLIB_TESTS)
    enable_testing()
endif()

# The builders of synthetic sound archives and executables for tests, and the checks of a conversion's output, beside
# xsflib's checks.
add_library(ntdlib_testing INTERFACE)
add_library(ntdlib::testing ALIAS ntdlib_testing)
target_include_directories(ntdlib_testing INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                                                    $<INSTALL_INTERFACE:include>)
target_link_libraries(ntdlib_testing INTERFACE xsflib::testing)
target_compile_features(ntdlib_testing INTERFACE cxx_std_20)
set_target_properties(ntdlib_testing PROPERTIES EXPORT_NAME testing)
set_property(GLOBAL APPEND PROPERTY NTDLIB_INSTALLED_TARGETS ntdlib_testing)

# The common files: the GameCube's and the Wii's addresses, the PowerPC's fused operations and float-to-integer
# conversion, AArch64's minimum and maximum, DOL executables, and the builds of the Wii's sound code that a main.dol
# links.
xsflib_add_project_library(ntdlib common src/common/dol.cpp src/common/sound_build.cpp)
target_link_libraries(ntdlib_common PUBLIC xsflib::common)
xsflib_add_project_test(ntdlib sound_build ntdlib::common)
xsflib_add_project_test(ntdlib aarch64 ntdlib::common)
xsflib_add_project_test(ntdlib fused ntdlib::common)

# The decoders of raw sample data (DSP-ADPCM, IMA-ADPCM, AFC, PCM8 and PCM16), and the decoding of a sound archive's
# wave to PCM.
xsflib_add_project_library(ntdlib wave src/wave/adpcm.cpp src/wave/decode.cpp src/wave/pcm.cpp)
target_link_libraries(ntdlib_wave PRIVATE xsflib::common)
xsflib_add_project_test(ntdlib wave ntdlib::wave)

# The readers of JAudio2's sound data: the start-up archive (BAA) with its sound table, sequence collection, wave
# systems and banks, and RARC archives. And the length analysis of a sound in game mode (LoopFinder and
# game_lengths.h).
xsflib_add_project_library(ntdlib jaudio src/jaudio/aaf.cpp src/jaudio/archive.cpp src/jaudio/bank.cpp
                           src/jaudio/loop_finder.cpp src/jaudio/rarc.cpp src/jaudio/wave_system.cpp)
target_link_libraries(ntdlib_jaudio PUBLIC xsflib::common)
xsflib_add_project_test(ntdlib jaudio ntdlib::jaudio)
xsflib_add_project_test(ntdlib aaf ntdlib::jaudio)
xsflib_add_project_test(ntdlib loop_finder ntdlib::jaudio)
xsflib_add_project_test(ntdlib game_lengths ntdlib::jaudio)

# The readers of NintendoWare's sound archives for the 3DS (CSAR), the Wii U and the Switch (FSAR), with their files and
# the decoding of their waves. The reader of SARC archives gives the group files that a game keeps in them.
xsflib_add_project_library(ntdlib sar src/sar/formats.cpp src/sar/decode.cpp src/sar/sarc.cpp)
target_link_libraries(ntdlib_sar PUBLIC ntdlib::common ntdlib::wave)
xsflib_add_project_test(ntdlib sar ntdlib::sar ntdlib::wave)
xsflib_add_project_test(ntdlib sar_stop_at_first_problem ntdlib::sar)
xsflib_add_project_test(ntdlib sar_set_archive ntdlib::sar)
xsflib_add_project_test(ntdlib length_cases ntdlib::sar)

# The archive mode's parts of the rip tools for the Wii U and the Switch: the sounds to rip, the data they require, the
# groups outside an archive, the folders of a game's archives, the timing helpers, and the libraries and minis.
if (TARGET xsflib::psf)
    xsflib_add_project_library(ntdlib rip_archive src/rip/archive.cpp)
    target_link_libraries(ntdlib_rip_archive PUBLIC ntdlib::sar xsflib::psf xsflib::rip)
    xsflib_add_project_test(ntdlib rip_archive ntdlib::rip_archive)
    xsflib_add_project_test(ntdlib rip_archive_mode ntdlib::rip_archive)
endif()

# The readers of NintendoWare for Revolution's sound archives for the Wii (RSAR), with their files and the decoding of
# their waves.
xsflib_add_project_library(ntdlib rsar src/rsar/formats.cpp src/rsar/decode.cpp)
target_link_libraries(ntdlib_rsar PUBLIC ntdlib::common ntdlib::wave)
xsflib_add_project_test(ntdlib rsar ntdlib::rsar)

# The conversion of Nintendo's sound data to seqlib's MIDI, SoundFont 2 and SFZ files.
if (NTDLIB_SEQUENCE_EXPORT)
    xsflib_add_project_library(ntdlib sequence_export
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
        src/sequence_export/passes.cpp
        src/sequence_export/performance.cpp
        src/sequence_export/performer.cpp
        src/sequence_export/rsar_instruments.cpp
        src/sequence_export/sf2_build.cpp
        src/sequence_export/sfz.cpp
        src/sequence_export/sfz_rsar.cpp
        src/sequence_export/sfz_track.cpp)
    target_link_libraries(ntdlib_sequence_export PUBLIC seqlib::seqlib ntdlib::common ntdlib::nwsnd ntdlib::sar
                                                        ntdlib::rsar xsflib::console)
    xsflib_add_project_test(ntdlib sequence_export_performance ntdlib::sequence_export)
    xsflib_add_project_test(ntdlib sequence_export_frame ntdlib::sequence_export)
    xsflib_add_project_test(ntdlib sequence_export ntdlib::sequence_export)
    xsflib_add_project_test(ntdlib sequence_export_revolution ntdlib::sequence_export)
    if (NTDLIB_TESTS)
        # The Wii's tests read wii_pan.h, a private header of sequence_export.
        target_include_directories(ntdlib_sequence_export_revolution_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    endif()
    xsflib_add_project_test(ntdlib sequence_export_switch ntdlib::sequence_export)
    xsflib_add_project_test(ntdlib sequence_export_ctr ntdlib::sequence_export)
endif()

# The PowerPC interpreter of the GameCube's Gekko, the Wii's Broadway and the Wii U's Espresso, for game mode. Its
# floating point follows FPSCR's rounding mode at run time.
xsflib_add_project_library(ntdlib ppc src/ppc/cpu.cpp src/ppc/fpu.cpp)
target_link_libraries(ntdlib_ppc PUBLIC ntdlib::common)
if (MSVC)
    set_source_files_properties(src/ppc/fpu.cpp PROPERTIES COMPILE_OPTIONS /fp:strict)
else()
    set_source_files_properties(src/ppc/fpu.cpp PROPERTIES COMPILE_OPTIONS -frounding-math)
endif()

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

# Disc images of the GameCube and the Wii, and a game's files on them: plain images (ISO, NKit), WBFS, WDF and extracted
# discs. It requires no other library.
xsflib_add_project_library(ntdlib disc src/disc/cx.cpp src/disc/encodings.cpp src/disc/file_table.cpp src/disc/game.cpp
                           src/disc/image.cpp src/disc/source.cpp src/disc/title.cpp)
target_link_libraries(ntdlib_disc PUBLIC ntdlib::common)
xsflib_add_project_test(ntdlib disc_options ntdlib::disc)
xsflib_add_project_test(ntdlib disc_title ntdlib::disc)
xsflib_add_project_test(ntdlib disc_text ntdlib::disc)

# The compressed containers of disc images: GCZ, CISO, WIA and RVZ. They require zlib, Zstandard, liblzma and bzip2.
if (NTDLIB_DISC)
    xsflib_add_project_library(ntdlib disc_containers src/disc/compression.cpp src/disc/containers.cpp
                               src/disc/junk.cpp)
    target_link_libraries(ntdlib_disc_containers PUBLIC ntdlib::disc PRIVATE xsflib::zlib xsflib::zstd xsflib::lzma
                                                                               xsflib::bzip2)
    xsflib_add_project_test(ntdlib disc_gamecube ntdlib::disc_containers xsflib::zlib xsflib::zstd xsflib::lzma
                            xsflib::bzip2)
    xsflib_add_project_test(ntdlib disc_wii ntdlib::disc_containers xsflib::zlib xsflib::zstd xsflib::lzma
                            xsflib::bzip2)
endif()

# The devices that the GameCube's Flipper and the Wii's Hollywood share, for game mode: the DSP interface with the ARAM
# and audio DMAs, the audio interface, the processor interface, the external interface with the clock and SRAM, and a
# disc made of files. MachineBase holds the parts of a machine that don't depend on its other devices. The game-mode
# source holds what game mode does with a set around a machine. The library also holds the host's side of the Z2
# start-up's parameters (drivers/jaudio/z2.h).
xsflib_add_project_library(ntdlib flipper src/flipper/audio_interface.cpp src/flipper/dsp_interface.cpp
                           src/flipper/exi.cpp src/flipper/file_disc.cpp src/flipper/game_source.cpp
                           src/flipper/z2_params.cpp)
target_link_libraries(ntdlib_flipper PUBLIC ntdlib::dsp ntdlib::ppc)
xsflib_add_project_test(ntdlib flipper ntdlib::flipper)
xsflib_add_project_test(ntdlib z2_params ntdlib::flipper)

# What the Wii's and the Wii U's AX libraries share: the constants, the voice allocation and the low-pass coefficients.
xsflib_add_project_library(ntdlib ax src/ax/lpf.cpp src/ax/voices.cpp)
target_link_libraries(ntdlib_ax PUBLIC xsflib::common)
xsflib_add_project_test(ntdlib ax ntdlib::ax)

# The model of NintendoWare's sound libraries (nw::snd in NintendoWare for Cafe, nn::atk on the Switch, the 3DS's
# nw::snd and the Wii's NW4R SND): their tables, each console's arithmetic, the envelope and the LFO, and the sequence
# player as a template on a policy.
xsflib_add_project_library(ntdlib nwsnd src/nwsnd/envelope.cpp src/nwsnd/util.cpp)
target_link_libraries(ntdlib_nwsnd PUBLIC ntdlib::common)
xsflib_add_project_test(ntdlib nwsnd ntdlib::nwsnd)
xsflib_add_project_test(ntdlib nwsnd_sequence ntdlib::nwsnd)
xsflib_add_project_test(ntdlib nwsnd_voice ntdlib::nwsnd)
xsflib_add_project_test(ntdlib nwsnd_channel ntdlib::nwsnd)
xsflib_add_project_test(ntdlib nwsnd_ctr ntdlib::nwsnd)
xsflib_add_project_test(ntdlib nwsnd_revolution ntdlib::nwsnd)

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

# The archive play test's main, for a test that plays a sound archive's sounds on a model of the sound library
# (ntdlib/testing/archive_play.h).
xsflib_add_project_test(ntdlib archive_play ntdlib::nwsnd)

if (NTDLIB_INSTALL)
    list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
    include(NtdlibInstall)
endif()
