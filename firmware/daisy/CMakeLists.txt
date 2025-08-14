cmake_minimum_required(VERSION 3.15)

# Set the project name
set(PROJECT_NAME wavex-daisy)
project(${PROJECT_NAME} C CXX ASM)

# Use C++17 for modern headers and defaults
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Ensure STM32 device macros are visible to all subdirs (HAL expects these)
add_definitions(
    -DSTM32H750xx
    -DUSE_HAL_DRIVER
    -DHSE_VALUE=16000000
    -DHSI_VALUE=16000000
    -DARM_MATH_CM7
)

# Add libDaisy and DaisySP
add_subdirectory(libs/libDaisy)
add_subdirectory(libs/DaisySP)

# Shared protocol (headers under firmware/shared)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../shared)

# Create our main executable
set(SOURCES
    src/main.cpp
    src/sampler.hpp
    src/cv_bus.hpp
    src/timebase.hpp
    ../shared/spi_protocol/protocol.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

# Link libraries
target_link_libraries(${PROJECT_NAME} 
    daisy 
    DaisySP
    # TODO: Add shared protocol library for Daisy
)

# Set target properties for STM32H750
target_compile_definitions(${PROJECT_NAME} PRIVATE
    -DSTM32H750xx
    -DHSE_VALUE=16000000
    -DHSI_VALUE=16000000
    -DUSE_HAL_DRIVER
    -DARM_MATH_CM7
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m7
    -mthumb
    -mfpu=fpv5-d16
    -mfloat-abi=hard
    -fdata-sections
    -ffunction-sections
    -Wall
    -fstack-usage
    -MMD
    -MP
    $<$<CONFIG:Debug>:-Og -g3 -ggdb>
    $<$<CONFIG:Release>:-Ofast -g0>
)

target_link_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m7
    -mthumb
    -mfpu=fpv5-d16
    -mfloat-abi=hard
    -fdata-sections
    -ffunction-sections
    -Wall
    -fstack-usage
    -MMD
    -MP
    -Wl,--gc-sections
    -Wl,--print-memory-usage
) 