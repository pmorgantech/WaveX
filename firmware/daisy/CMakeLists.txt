cmake_minimum_required(VERSION 3.15)

# Set the project name
set(PROJECT_NAME wavex-daisy)
project(${PROJECT_NAME} C CXX ASM)

# Add libDaisy and DaisySP
add_subdirectory(libs/libDaisy)
add_subdirectory(libs/DaisySP)

# Add shared components (will be built separately for Daisy)
# add_subdirectory(../shared shared)

# Create our main executable
set(SOURCES
    src/main.cpp
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
    --specs=nano.specs
    --specs=nosys.specs
    -Wl,--gc-sections
    -Wl,--print-memory-usage
) 