cmake_minimum_required(VERSION 3.20)

# Set the project name
project(wavex-daisy)

# Set the project name and sources
set(FIRMWARE_NAME wavex-daisy)
set(FIRMWARE_SOURCES
    src/main.cpp
    src/sampler.hpp
    src/cv_bus.hpp
    src/timebase.hpp
    ../shared/spi_protocol/protocol.cpp
)

# Set Daisy storage to QSPI for more flash memory (7.75MB vs 128KB)
set(DAISY_STORAGE qspi CACHE STRING "Select the storage destination of the executable.")
# Use Daisy's QSPI placement (sets sections/ld script appropriately)
set(DAISY_USE_QSPI_FLASH ON)
# Build for bootloader QSPI (not internal flash)
set(APP_TYPE BOOT_QSPI)
# Set the toolchain to use STM32H750
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/libs/libDaisy/cmake/toolchains/ArmGNUToolchain.cmake)
# Add libDaisy and DaisySP
add_subdirectory(libs/libDaisy)
add_subdirectory(libs/DaisySP)

# Define DAISYSP_LIB for DaisyProject.cmake
set(DAISYSP_LIB DaisySP)

# Shared protocol (headers under firmware/shared)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../shared)

# Add DaisySP headers to include path
include_directories(${CMAKE_CURRENT_LIST_DIR}/libs/DaisySP/Source)

# Include DaisyProject.cmake which handles all the STM32 configuration
include(DaisyProject) 