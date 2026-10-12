# SPDX-License-Identifier: MIT

# A project that uses ntdlib, to check the CMake package. NTDLIB_ROUTE selects how the project gets ntdlib:
# "subdirectory" adds the source folder, "fetchcontent" declares it with FetchContent, and "find_package" finds an
# installed copy. In the first two, the project adds xsflib from ntdlib's submodule first. A project that uses both
# libraries does the same.
cmake_minimum_required(VERSION 3.20)
project(ntdlib_package_test LANGUAGES C CXX)

set(NTDLIB_ROUTE "find_package" CACHE STRING "How to get ntdlib: subdirectory, fetchcontent or find_package")
set(NTDLIB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "ntdlib's source folder")

if (NTDLIB_ROUTE STREQUAL "subdirectory")
    add_subdirectory(${NTDLIB_SOURCE_DIR}/externals/xsflib xsflib EXCLUDE_FROM_ALL)
    add_subdirectory(${NTDLIB_SOURCE_DIR} ntdlib EXCLUDE_FROM_ALL)
elseif (NTDLIB_ROUTE STREQUAL "fetchcontent")
    include(FetchContent)
    FetchContent_Declare(xsflib SOURCE_DIR ${NTDLIB_SOURCE_DIR}/externals/xsflib)
    FetchContent_Declare(ntdlib SOURCE_DIR ${NTDLIB_SOURCE_DIR})
    FetchContent_MakeAvailable(xsflib ntdlib)
elseif (NTDLIB_ROUTE STREQUAL "find_package")
    find_package(ntdlib 1.0 REQUIRED)
else()
    message(FATAL_ERROR "NTDLIB_ROUTE is ${NTDLIB_ROUTE}: use subdirectory, fetchcontent or find_package")
endif()

add_executable(package_test main.cpp)
target_link_libraries(package_test PRIVATE ntdlib::wave xsflib::testing)
set_target_properties(package_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

enable_testing()
add_test(NAME package_test COMMAND package_test)
