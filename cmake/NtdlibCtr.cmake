# SPDX-License-Identifier: MIT

# The 3DS's DSP and archive mode's model of the 3DS's sound libraries on it (ntdlib::ctr). The DSP is a model of the
# DSP's audio firmware, which requires no image, or Teakra running a DSP1 firmware image, such as a game's
# dspaudio.cdc. The program that uses the module supplies the image. The glue drives Teakra from the ARM11's side. The
# models of nn::snd and nw::snd play a sound archive's sequences on either. CMakeLists.txt includes this file.
#
# The package that NTDLIB_INSTALL installs holds no Teakra yet. A build that installs ntdlib leaves the module out.
#
# The module is on when ntdlib is built alone. A project that adds ntdlib turns it on when it uses the module. Some
# projects add Teakra after ntdlib, and a second target of the same name breaks their configure.

include(CMakeDependentOption)
cmake_dependent_option(NTDLIB_CTR "Build ntdlib::ctr, the 3DS's DSP in Teakra and archive mode's model on it."
                       ${NTDLIB_TOP_LEVEL} "NOT NTDLIB_INSTALL" OFF)
if (NOT NTDLIB_CTR)
    return()
endif()

# Teakra (MIT): the emulator of the Teak DSP. externals/teakra is upstream commit 3d697a1 with changes
# (THIRD-PARTY-NOTICES.md). Teakra sets CMAKE_WARN_DEPRECATED, which CMake 4.4 and later ignore and warn about
# (CMP0218).
if (NOT TARGET teakra)
    set(TEAKRA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(TEAKRA_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_DEFAULT_CMP0218 NEW)
    add_subdirectory(${PROJECT_SOURCE_DIR}/externals/teakra ${PROJECT_BINARY_DIR}/externals/teakra EXCLUDE_FROM_ALL)

    # The interpreter has a handler for each value of the common instructions' register operands: thousands of
    # functions in one object file, which can be more sections than a Windows object file holds by default.
    if (MSVC)
        target_compile_options(teakra PRIVATE /bigobj)
    endif()
endif()

xsflib_add_project_library(ntdlib ctr src/ctr/dsp/firmware_audio.cpp src/ctr/dsp/firmware_enhanced.cpp
                           src/ctr/dsp/firmware_model.cpp src/ctr/dsp/teakra_dsp.cpp src/ctr/nnsnd/dsp_link.cpp
                           src/ctr/nnsnd/sound_system.cpp src/ctr/nnsnd/voice.cpp src/ctr/snd/engine.cpp
                           src/ctr/snd/sequence.cpp src/ctr/snd/sound_players.cpp src/ctr/snd/util.cpp
                           src/ctr/snd/voice.cpp)
target_link_libraries(ntdlib_ctr PUBLIC ntdlib::nwsnd ntdlib::sar PRIVATE ntdlib::wave teakra)

# Teakra's checks of its bounds, its state and its two ways of running, and the glue's checks on small firmwares.
# Archive mode's test plays on the model of the firmware, and also on Teakra running a firmware that the environment
# variable NTDLIB_CTR_FIRMWARE names.
xsflib_add_project_test(ntdlib ctr_teakra_bounds teakra)
xsflib_add_project_test(ntdlib ctr_teakra_dsp ntdlib::ctr)
xsflib_add_project_test(ntdlib ctr_teakra_jit teakra)
xsflib_add_project_test(ntdlib ctr_teakra_state teakra)
xsflib_add_project_test(ntdlib ctr_snd ntdlib::ctr)
xsflib_add_project_test(ntdlib ctr_enhanced ntdlib::ctr)
