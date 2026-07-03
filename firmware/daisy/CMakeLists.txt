cmake_minimum_required(VERSION 3.20)

# Set the toolchain before declaring the project so it takes effect.
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/libs/libDaisy/cmake/toolchains/ArmGNUToolchain.cmake)

# Project version is the repo-root VERSION file, shared with the ESP32 build
# so frontend and backend firmware stay in sync. Bump that one file, not this one.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" WAVEX_VERSION)
project(wavex-daisy VERSION ${WAVEX_VERSION})

# Set the project name and sources
set(FIRMWARE_NAME wavex-daisy)
set(FIRMWARE_SOURCES
    src/main.cpp
    src/config.hpp
    src/comm/daisy_spi_link.cpp
    src/comm/daisy_inter_mcu_message_handlers.cpp
    src/comm/daisy_uart_link.cpp
    src/comm/daisy_logging.cpp
    src/comm/daisy_filesystem.cpp
    src/audio/audio_adapter.cpp
    src/audio/audio_engine.cpp
    src/storage/fs_browse.cpp
    src/storage/sd_sdio.cpp
    src/storage/sd_log_bridge.cpp
    src/metrics/metrics.cpp
    src/sampler.hpp
    src/audio/output_sink.hpp
    src/audio/voice_manager.hpp
    src/audio/envelope.hpp
    src/audio/one_pole_filter.hpp
    src/cv/cv_cal.hpp
    src/cv/cv_group_router.hpp
    src/cv/mcp4728_backend.hpp
    src/cv/mcp48_backend.hpp
    src/timebase.hpp
    src/profiling/profiler.cpp
    ../shared/spi_protocol/protocol.cpp
    ../shared/uart_protocol/uart_protocol.cpp
)

# Set Daisy storage to QSPI for more flash memory (7.75MB vs 128KB)
set(DAISY_STORAGE qspi CACHE STRING "Select the storage destination of the executable.")
# Use Daisy's QSPI placement (sets sections/ld script appropriately)
set(DAISY_USE_QSPI_FLASH ON)
# Build for bootloader QSPI (not internal flash)
set(APP_TYPE BOOT_QSPI)
# Add libDaisy and DaisySP
add_subdirectory(libs/libDaisy)
add_subdirectory(libs/DaisySP)

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

# Define DAISYSP_LIB for DaisyProject.cmake
set(DAISYSP_LIB DaisySP)

# Shared protocol (headers under firmware/shared)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../shared)
# Include src directory for relative includes
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/storage)

# Add DaisySP headers to include path
include_directories(${CMAKE_CURRENT_LIST_DIR}/libs/DaisySP/Source)

# Add definitions for the project
add_compile_definitions(
    DAISY_PLATFORM
    HSE_VALUE=16000000
    ARM_MATH_CM7
    __FPU_PRESENT=1
)

# Profiling option
option(WAVEX_PROFILING_ENABLED "Enable the STM32H7 DWT/ITM profiling helpers" OFF)
if(WAVEX_PROFILING_ENABLED)
    add_compile_definitions(WAVEX_PROFILING_ENABLED=1)
else()
    add_compile_definitions(WAVEX_PROFILING_ENABLED=0)
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

set(CMSIS_DSP_DIR ${CMAKE_CURRENT_LIST_DIR}/libs/libDaisy/Drivers/CMSIS-DSP)

# Explicitly list the CMSIS-DSP sources we require (present in libDaisy’s CMSIS tree).
set(CMSIS_DSP_SOURCES
    ${CMSIS_DSP_DIR}/Source/SupportFunctions/arm_float_to_f16.c
    ${CMSIS_DSP_DIR}/Source/SupportFunctions/arm_f16_to_float.c
    ${CMSIS_DSP_DIR}/Source/SupportFunctions/arm_float_to_q15.c
    ${CMSIS_DSP_DIR}/Source/SupportFunctions/arm_float_to_q31.c
    ${CMSIS_DSP_DIR}/Source/SupportFunctions/arm_float_to_q7.c
    ${CMSIS_DSP_DIR}/Source/SupportFunctions/arm_q15_to_float.c
    ${CMSIS_DSP_DIR}/Source/SupportFunctions/arm_q31_to_float.c
    ${CMSIS_DSP_DIR}/Source/SupportFunctions/arm_q7_to_float.c
    ${CMSIS_DSP_DIR}/Source/SupportFunctions/arm_copy_q15.c
    ${CMSIS_DSP_DIR}/Source/InterpolationFunctions/arm_linear_interp_q15.c
)

add_library(cmsis_dsp_support OBJECT ${CMSIS_DSP_SOURCES})
target_include_directories(cmsis_dsp_support PUBLIC ${CMSIS_DSP_DIR}/Include)
target_include_directories(cmsis_dsp_support PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/libs/libDaisy/Drivers/CMSIS_5/CMSIS/Core/Include
    ${CMAKE_CURRENT_LIST_DIR}/libs/libDaisy/Drivers/CMSIS_5/CMSIS/Include
)
target_link_libraries(${FIRMWARE_NAME} PRIVATE cmsis_dsp_support)

# Set linker specs to use nano library with nosys stubs
set(CMAKE_EXE_LINKER_FLAGS "--specs=nano.specs --specs=nosys.specs")
