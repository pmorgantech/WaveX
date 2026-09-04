cmake_minimum_required(VERSION 3.20)

# Set the toolchain before declaring the project so it takes effect.
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/libs/libDaisy/cmake/toolchains/ArmGNUToolchain.cmake)

# Project version is the repo-root VERSION file, shared with the ESP32 build
# so frontend and backend firmware stay in sync. Bump that one file, not this one.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" WAVEX_VERSION)
project(wavex-daisy VERSION ${WAVEX_VERSION})

# Optimization level for the Daisy image.
#
# This has to live here. libDaisy's toolchain file sets
# CMAKE_{C,CXX}_FLAGS_{DEBUG,RELEASE} to "" as CACHE INTERNAL - a forced cache
# write - so -DCMAKE_BUILD_TYPE=Release contributes nothing and a
# -DCMAKE_CXX_FLAGS_RELEASE=... on the command line is overwritten on every
# configure. Nothing outside this file can set it.
#
# The default is -O0 because that is what this firmware has always been built
# with (unintentionally: no -O was ever specified, and -finline-functions is
# inert without one). Raising it changes real-time behaviour everywhere and can
# expose undefined behaviour that -O0 was masking, so it is opt-in rather than
# a silent default change - see docs/field-findings-20260829.md §1.
#
# To A/B it without disturbing the current build:
#   cd firmware/daisy && make BUILD_DIR=build-O2 CMAKE_EXTRA_ARGS="-DWAVEX_DAISY_OPT=-O2"
#
# docs/daisy_rt_audio_coding_guide.md §8 asks for -O3; treat -O2 as the first
# step and measure each move with DWT rather than assuming.
set(WAVEX_DAISY_OPT "-O0" CACHE STRING "Optimization flag for the Daisy firmware (e.g. -O0, -O2, -O3)")
add_compile_options(${WAVEX_DAISY_OPT})
message(STATUS "WaveX Daisy optimization: ${WAVEX_DAISY_OPT}")

# Set the project name and sources
set(FIRMWARE_NAME wavex-daisy)
set(FIRMWARE_SOURCES
    src/main.cpp
    src/config.hpp
    src/comm/daisy_spi_link.cpp
    src/comm/daisy_inter_mcu_message_handlers.cpp
    src/comm/daisy_uart_link.cpp
    src/comm/diag_push.cpp
    src/comm/uart4_dma_transport.cpp
    src/comm/daisy_logging.cpp
    src/comm/log_ring.cpp
    src/comm/daisy_filesystem.cpp
    src/audio/audio_engine.cpp
    src/audio/note_event_queue.hpp
    src/audio/sample_load_info.hpp
    src/audio/sfz_import.hpp
    src/audio/sfz_loader.cpp
    src/audio/sfz_loader.hpp
    src/audio/snapshot_mailbox.hpp
    src/storage/fs_browse.cpp
    src/storage/sd_sdio.cpp
    src/memory_sections.cpp
    src/audio/output_sink.hpp
    src/audio/voice_manager.hpp
    src/audio/instrument.hpp
    src/audio/envelope.hpp
    src/audio/svf_filter.hpp
    src/cv/cv_cal.hpp
    src/cv/cv_group_router.hpp
    src/cv/mcp4728_backend.hpp
    src/cv/mcp48_backend.hpp
    src/sequencer/pattern.hpp
    src/sequencer/sequencer_scheduler.hpp
    src/sequencer/tempo_follower.hpp
    src/sequencer/sequencer_transport.hpp
    src/timebase.hpp
    src/profiling/profiler.cpp
    ../shared/spi_protocol/protocol.cpp
    ../shared/uart_protocol/uart_protocol.cpp
)

# QSPI remains the persistent/default image. The fast SWD iteration target
# configures a separate build tree with DAISY_STORAGE=sram and this option ON;
# never share a CMake cache between the two linker layouts.
set(DAISY_STORAGE "qspi" CACHE STRING "libDaisy executable storage (qspi or sram)")
set_property(CACHE DAISY_STORAGE PROPERTY STRINGS qspi sram)
option(WAVEX_SRAM_DEBUG "Build a self-starting SRAM image for direct SWD loading" OFF)

if(WAVEX_SRAM_DEBUG)
    if(NOT DAISY_STORAGE STREQUAL "sram")
        message(FATAL_ERROR "WAVEX_SRAM_DEBUG requires -DDAISY_STORAGE=sram")
    endif()

    # DaisyProject normally treats every non-flash image as a bootloader app
    # and defines BOOT_APP, which skips SystemInit(). A direct SWD load bypasses
    # the Daisy bootloader, so use the SRAM layout as a custom script and make
    # the image perform its own reset-time clock/vector setup instead.
    set(CUSTOM_LINKER_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/wavex_sram_debug.lds")
    list(APPEND FIRMWARE_SOURCES src/sram_debug_startup.cpp)
elseif(NOT DAISY_STORAGE STREQUAL "qspi")
    message(FATAL_ERROR "Persistent WaveX builds require DAISY_STORAGE=qspi")
endif()

# Add libDaisy. DaisySP stays a submodule but is not built or linked: nothing
# in the image uses it (the per-voice envelope, SVF, LFO and fades are the
# small first-party header-only kernels in src/audio, chosen so the host tests
# compile them directly). To use a DaisySP module, add
# `add_subdirectory(libs/DaisySP)`, `set(DAISYSP_LIB DaisySP)` and its Source
# include path back; the MIT/LGPL split of its modules is in docs/LICENSES.md.
add_subdirectory(libs/libDaisy)

if(WAVEX_SRAM_DEBUG)
    # The SWD loader establishes SP before entering Reset_Handler. Do not use
    # libDaisy's sram_layout SP reload here: at -O0 it runs after the C function
    # prologue and moves compiler locals past the top of DTCM.
    # SystemInit() still needs to point VTOR at the vector table loaded at
    # 0x24000000 and enable D2 before the preinit hook clears WaveX state.
    target_compile_definitions(
        CMSIS_DEVICE_H7 PRIVATE DATA_IN_D2_SRAM USER_VECT_TAB_ADDRESS VECT_TAB_SRAM
    )
endif()

# libDaisy v8.1.0's uart.cpp owns global UART/DMA IRQ callbacks and uses one
# global DMA scheduler, which cannot transmit while circular RX listen mode is
# active (upstream issue #653). WaveX owns UART4 through
# uart4_dma_transport.cpp instead, so omit that one driver translation unit
# without modifying the pinned libDaisy submodule. Any future use of
# daisy::UartHandler in this target will fail to link and must be reconciled
# explicitly with the WaveX-owned UART4 transport.
get_target_property(LIBDAISY_SOURCES daisy SOURCES)
list(FILTER LIBDAISY_SOURCES EXCLUDE REGEX "[/\\\\]per[/\\\\]uart\\.cpp$")
set_property(TARGET daisy PROPERTY SOURCES "${LIBDAISY_SOURCES}")

# ===== FIX HAL DRIVER CMSIS INCLUDES =====
# libDaisy v8.0.0 HAL driver target is missing CMSIS Core includes
# Add them explicitly so uint32_t/etc. are available in the HAL headers.
set(CMSIS_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/libs/libDaisy/Drivers/CMSIS/Include
    ${CMAKE_CURRENT_LIST_DIR}/libs/libDaisy/Drivers/CMSIS_5/CMSIS/Include
    ${CMAKE_CURRENT_LIST_DIR}/libs/libDaisy/Drivers/CMSIS_5/CMSIS/Core/Include
)
set(CMSIS_DEVICE_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/libs/libDaisy/Drivers/CMSIS/Device/ST/STM32H7xx/Include
    ${CMAKE_CURRENT_LIST_DIR}/libs/libDaisy/Drivers/CMSIS-Device/ST/STM32H7xx/Include
)
set(HAL_CMSIS_INCLUDES "")
foreach(dir IN LISTS CMSIS_INCLUDE_DIRS CMSIS_DEVICE_INCLUDE_DIRS)
    if(EXISTS ${dir})
        list(APPEND HAL_CMSIS_INCLUDES ${dir})
    endif()
endforeach()
if(TARGET STM32H7xx_HAL_Driver AND HAL_CMSIS_INCLUDES)
    target_include_directories(STM32H7xx_HAL_Driver PRIVATE ${HAL_CMSIS_INCLUDES})
endif()
# =========================================

# DaisyProject.cmake links ${DAISYSP_LIB}; empty because DaisySP is not built
# (see the add_subdirectory note above).
set(DAISYSP_LIB "")

# Shared protocol (headers under firmware/shared)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../shared)
# Include src directory for relative includes
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/storage)

# Add definitions for the project
add_compile_definitions(
    DAISY_PLATFORM
    HSE_VALUE=16000000
    ARM_MATH_CM7
    # 1U, not 1: must match stm32h750xx.h's own definition textually, or every
    # TU that includes the device header warns "__FPU_PRESENT" redefined.
    __FPU_PRESENT=1U
)

# Profiling option
option(WAVEX_PROFILING_ENABLED "Enable the STM32H7 DWT/ITM profiling helpers" OFF)
if(WAVEX_PROFILING_ENABLED)
    add_compile_definitions(WAVEX_PROFILING_ENABLED=1)
else()
    add_compile_definitions(WAVEX_PROFILING_ENABLED=0)
endif()

# Debug/release build profile (README.md#build-profiles). Master
# switch for the debug feature surface: logging_config.h defaults
# WAVEX_DEBUG_LOGGING_ENABLED and WAVEX_DEBUG_HARNESS_ENABLED from it, so
# `make BUILD_DIR=build-release CMAKE_EXTRA_ARGS="-DWAVEX_BUILD_DEBUG=OFF"`
# drops the console command surface from the image. Always emits a definition
# rather than leaving the header's #ifndef default standing, so the value in
# force is the one this build chose.
#
# Use a separate BUILD_DIR per profile: the wrapper Makefile only runs the
# CMake configure step when CMakeCache.txt is absent, so reusing a build
# directory silently keeps the previous profile's flags.
option(WAVEX_BUILD_DEBUG "Debug profile: logging and the console harness" ON)
if(WAVEX_BUILD_DEBUG)
    add_compile_definitions(WAVEX_BUILD_DEBUG=1)
else()
    add_compile_definitions(WAVEX_BUILD_DEBUG=0)
endif()

# Voice output / CV backend selection (architecture.md §5.3, roadmap Phase 1
# item 1). Empty (the default) leaves hardware_config.h's own #ifndef
# defaults (Stage A) in effect. CI uses these to also compile the Stage B
# flag set without needing a second checkout - e.g.:
#   cmake -DWAVEX_VOICE_OUTPUT_BACKEND=1 -DWAVEX_CV_BACKEND=1 -DWAVEX_ANALOG_CV_GROUPS=8 ..
set(WAVEX_VOICE_OUTPUT_BACKEND "" CACHE STRING "0=StereoMix (Stage A), 1=TDM8 (Stage B)")
set(WAVEX_CV_BACKEND "" CACHE STRING "0=MCP4728 (Stage A), 1=MCP48 chain (Stage B)")
set(WAVEX_ANALOG_CV_GROUPS "" CACHE STRING "1=paraphonic (Stage A), 8=full voice board (Stage B)")
if(NOT WAVEX_VOICE_OUTPUT_BACKEND STREQUAL "")
    add_compile_definitions(WAVEX_VOICE_OUTPUT_BACKEND=${WAVEX_VOICE_OUTPUT_BACKEND})
endif()
if(NOT WAVEX_CV_BACKEND STREQUAL "")
    add_compile_definitions(WAVEX_CV_BACKEND=${WAVEX_CV_BACKEND})
endif()
if(NOT WAVEX_ANALOG_CV_GROUPS STREQUAL "")
    add_compile_definitions(WAVEX_ANALOG_CV_GROUPS=${WAVEX_ANALOG_CV_GROUPS})
endif()


# Include DaisyProject.cmake which handles all the STM32 configuration
include(DaisyProject)

# Shared logging state uses C++17 inline variables. Select the language level
# explicitly instead of relying on the ARM toolchain's current gnu++14 default.
set_target_properties(${FIRMWARE_NAME} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS YES
)

# First-party warning flags, matching the host test suites (see
# docs/testing-remediation.md). Warnings, not -Werror: CI surfaces them, and a
# hard failure on a toolchain bump is worse than a visible warning. Scoped to
# our executable target only; the vendor targets below are marked SYSTEM so
# their interface headers are consumed via -isystem and do not drown the
# first-party signal (libDaisy and the STM32 HAL do not build clean under
# -Wconversion).
#
# -Wundef is here for a specific defect class this project has already hit: a
# `#if WAVEX_SOMETHING` whose macro is not defined evaluates to 0 silently, so
# a feature guard whose flag was renamed or never included compiles the feature
# OUT with no diagnostic and no build failure. That happened at 07c68c3, where
# main.cpp's guards landed a commit ahead of the header defining them. It is
# safe here because first-party Daisy code always defines its flags (the
# #ifndef/#define default pattern in hardware_config.h and logging_config.h)
# and builds clean under it - unlike the ESP32, where `#if CONFIG_FOO` on an
# unset Kconfig bool is the idiomatic spelling and -Wundef is unusable.
target_compile_options(${FIRMWARE_NAME} PRIVATE -Wall -Wextra -Wconversion -Wundef)
foreach(vendor_target daisy STM32H7xx_HAL_Driver)
    if(TARGET ${vendor_target})
        set_target_properties(${vendor_target} PROPERTIES SYSTEM TRUE)
    endif()
endforeach()

# The STM32 HAL headers reach us on a plain -I: they come from libDaisy's
# directory-scoped include_directories(), which the SYSTEM property on the
# targets above does not cover. Without this they contribute ~24 -Wundef
# warnings of their own and drown the first-party signal the flag exists for.
target_include_directories(${FIRMWARE_NAME} SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/libDaisy/Drivers/STM32H7xx_HAL_Driver/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/libDaisy/Drivers/STM32H7xx_HAL_Driver/Inc/Legacy
)

if(NOT WAVEX_SRAM_DEBUG)
    # The SRAM debug linker script already contains these WaveX-specific
    # sections and assertions, with SRAM rather than QSPI load addresses.
    target_link_options(${FIRMWARE_NAME} PRIVATE
        "-Wl,-T,${CMAKE_CURRENT_LIST_DIR}/wavex_memory_sections.ld"
    )
endif()

# CMSIS-DSP is not linked. The sources live in libs/libDaisy/Drivers/CMSIS-DSP;
# when a kernel is worth it (FFT, polyphase FIR resampling, saturating q15
# block math - docs/daisy_rt_audio_coding_guide.md §8), add an OBJECT library
# of exactly the arm_*.c files used, with Include/ and the CMSIS_5 Core
# include on its interface, and link it PRIVATE. An earlier shim compiled ten
# conversion/interpolation files of which the link kept one plain copy loop.

# Set linker specs to use nano library with nosys stubs
set(CMAKE_EXE_LINKER_FLAGS "--specs=nano.specs --specs=nosys.specs")
