# PS4 (OpenOrbis) build of DKC2Recomp.
#
#   cmake -S ps4 -B build-ps4 -G Ninja -DCMAKE_TOOLCHAIN_FILE=ps4/openorbis.cmake
#   cmake --build build-ps4
#
# The portable snesrecomp runtime and the DKC2 game layer are compiled into
# dkc2_ps4_core. The native host (ps4/src) links it with the private generated
# C from generated/snesrecomp (scripts/generate_snesrecomp.py, from your own
# ROM). Without generated sources only the core library is built, which is
# enough to check that everything compiles for the console.
cmake_minimum_required(VERSION 3.16)
project(DKC2RecompPS4 VERSION 0.0.8 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(DKC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(SNESRECOMP_ROOT "${DKC2_ROOT}/snesrecomp")
set(DKC2_SNESRECOMP_GEN_DIR "${DKC2_ROOT}/generated/snesrecomp" CACHE PATH
    "Directory containing private generated snesrecomp C sources")

# runner.cmake declares SDL helper functions but only uses them when called.
set(SNESRECOMP_SDL_BACKEND SDL2 CACHE STRING "" FORCE)
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

# The Project Kongs OAM hooks patch a build-local copy of ppu.c. The include
# expects CMAKE_CURRENT_SOURCE_DIR to be the repository root.
set(_dkc2_saved_source_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(CMAKE_CURRENT_SOURCE_DIR "${DKC2_ROOT}")
include(${DKC2_ROOT}/cmake/ProjectKongsPpu.cmake)
set(CMAKE_CURRENT_SOURCE_DIR "${_dkc2_saved_source_dir}")

# Hosts without SDL provide these themselves (see runner/headless_host.c).
list(REMOVE_ITEM SNESRECOMP_RUNNER_SOURCES
    "${SNESRECOMP_ROOT}/runner/src/keybinds.c"
    "${SNESRECOMP_ROOT}/runner/src/host_report.c")

set(DKC2_PS4_DEFINITIONS
    SNESRECOMP_TRACE=0
    SNESRECOMP_REVERSE_DEBUG=0
    SNESRECOMP_EXTERNAL_RAM_ROUTINE_GUARDS=1
    SYSTEM_VOLUME_MIXER_AVAILABLE=0
    DKC2_PS4=1
    _GNU_SOURCE)

set(DKC2_PS4_INCLUDES
    ${DKC2_ROOT}/runner
    ${DKC2_ROOT}/recomp
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS})

add_library(dkc2_ps4_core STATIC
    ${SNESRECOMP_RUNNER_SOURCES}
    ${DKC2_ROOT}/runner/dkc2_music.c
    ${DKC2_ROOT}/runner/dkc2_msu1.c
    ${DKC2_ROOT}/runner/dkc2_spc_music.c
    ${DKC2_ROOT}/runner/dkc2_game.c
    ${DKC2_ROOT}/runner/dkc2_coop.c
    ${DKC2_ROOT}/runner/dkc2_video.c
    ${DKC2_ROOT}/runner/dkc2_hdma.c
    ${DKC2_ROOT}/runner/verified_rom.c)
target_include_directories(dkc2_ps4_core PUBLIC ${DKC2_PS4_INCLUDES})
target_compile_definitions(dkc2_ps4_core PUBLIC ${DKC2_PS4_DEFINITIONS})
target_compile_options(dkc2_ps4_core PRIVATE -O2 -Wno-everything)

file(GLOB DKC2_SNESRECOMP_GEN_SOURCES CONFIGURE_DEPENDS
    "${DKC2_SNESRECOMP_GEN_DIR}/*.c")

if(NOT DKC2_SNESRECOMP_GEN_SOURCES)
    message(WARNING
        "No generated sources in ${DKC2_SNESRECOMP_GEN_DIR}: building only "
        "dkc2_ps4_core. Run scripts/generate_snesrecomp.py with your ROM.")
    return()
endif()

# Same generated-code integration steps as the desktop build.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
foreach(_script apply_dkc2_kongs_overrides.py apply_dkc2_music_overrides.py)
    execute_process(COMMAND "${Python3_EXECUTABLE}"
        "${DKC2_ROOT}/scripts/${_script}"
        --generated-dir "${DKC2_SNESRECOMP_GEN_DIR}"
        RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${_script} failed")
    endif()
endforeach()

add_executable(dkc2_ps4
    ${DKC2_SNESRECOMP_GEN_SOURCES}
    ${DKC2_ROOT}/runner/headless_host.c
    src/main.c
    src/gnm.c)
target_include_directories(dkc2_ps4 PRIVATE src ${DKC2_PS4_INCLUDES})
target_compile_options(dkc2_ps4 PRIVATE -O2 -Wno-everything)
target_link_libraries(dkc2_ps4 PRIVATE
    dkc2_ps4_core
    SceVideoOut SceAudioOut ScePad SceUserService SceSystemService
    SceSysmodule SceGnmDriver m)
