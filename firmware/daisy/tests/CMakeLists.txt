cmake_minimum_required(VERSION 3.16)
project(wavex-daisy-test)

# Only build tests if not cross-compiling
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Skipping tests - cross-compiling")
    return()
endif()

find_package(Threads REQUIRED)

# Set C++ standard before adding GoogleTest
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GoogleTest is vendored as a submodule (firmware/shared/tests/_deps/googletest-src)
# and shared across all three test suites, so `make test` works offline and
# doesn't refetch on every fresh build dir.
include(FetchContent)
FetchContent_Declare(
    googletest
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/tests/_deps/googletest-src
)
# Prevent GoogleTest from overriding our C++ standard
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Ensure GoogleTest targets use C++14
if(TARGET gtest)
    set_target_properties(gtest PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED YES
    )
endif()
if(TARGET gtest_main)
    set_target_properties(gtest_main PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED YES
    )
endif()

# Include directories
# Note: mocks directory must come before libDaisy to override ff.h and daisy_seed.h
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks  # Must be first to override ff.h and daisy_seed.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/comm
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/storage
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/metrics
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/spi_protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/uart_protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/../libs/libDaisy/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../libs/DaisySP/Source
)

# Test library with WaveX source files (excluding hardware-dependent code)
# Only include components that can be tested without full hardware dependencies
add_library(wavex_test_lib STATIC
    # Metrics is self-contained and testable
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/metrics/metrics.cpp
    # Storage filesystem browsing (uses FatFS mock)
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/storage/fs_browse.cpp
    # FatFS mock implementation
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/fatfs_mock.cpp
    # Sampler is self-contained and testable
    # Note: audio_engine.cpp and other hardware-dependent files excluded for now
    # They require STM32 HAL headers which aren't available in host tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/uart_protocol/uart_protocol.cpp
)

target_include_directories(wavex_test_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
    ${CMAKE_CURRENT_SOURCE_DIR}/../libs/libDaisy/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../libs/DaisySP/Source
)

target_compile_definitions(wavex_test_lib PUBLIC
    UNIT_TEST
    DAISY_PLATFORM
)

# Link against libDaisy test library if available
if(TARGET daisy)
    target_link_libraries(wavex_test_lib PUBLIC daisy)
endif()

target_link_libraries(wavex_test_lib PUBLIC
    Threads::Threads
    GTest::gtest
)

set_target_properties(wavex_test_lib PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED YES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
)

enable_testing()

# Function to add a test executable
function(add_wavex_test test_source)
    cmake_path(GET test_source STEM test_name)

    add_executable(${test_name} ${test_source})

    # For sampler test, add sampler.hpp directly (it's header-only with inline implementation)
    if(test_name STREQUAL "sampler_test")
        target_sources(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/sampler.hpp
        )
    endif()

    target_link_libraries(${test_name} PUBLIC
        wavex_test_lib
        Threads::Threads
        GTest::gtest
        GTest::gtest_main
    )

    target_compile_definitions(${test_name} PRIVATE
        UNIT_TEST
        DAISY_PLATFORM
    )

    set_target_properties(${test_name} PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED YES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    gtest_discover_tests(${test_name})
endfunction()

# Auto-discover and add all test files
file(GLOB_RECURSE unit_tests "${CMAKE_CURRENT_SOURCE_DIR}/unit/*_test.cpp")
file(GLOB_RECURSE integration_tests "${CMAKE_CURRENT_SOURCE_DIR}/integration/*_test.cpp")

# Filter out hardware-dependent tests that require STM32 HAL headers
# These tests need full hardware mocks or should be run on target hardware
list(FILTER unit_tests EXCLUDE REGEX ".*audio_engine_test\\.cpp$")
list(FILTER unit_tests EXCLUDE REGEX ".*daisy_uart_link_test\\.cpp$")

foreach(test_file IN LISTS unit_tests integration_tests)
    add_wavex_test(${test_file})
endforeach()

# Note: Hardware-dependent tests (audio_engine_test, daisy_uart_link_test) are excluded
# from host-based testing as they require STM32 HAL headers. These should be:
# 1. Run on target hardware, or
# 2. Refactored with better mocks to avoid hardware dependencies
