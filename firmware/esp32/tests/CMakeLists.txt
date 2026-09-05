cmake_minimum_required(VERSION 3.16)
project(wavex-esp32-test)

# Only build tests if not cross-compiling
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Skipping tests - cross-compiling")
    return()
endif()

find_package(Threads REQUIRED)

# Set C++ standard before adding GoogleTest
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Address/UB sanitizers, off by default so the default `make test` stays fast.
# Enable with -DWAVEX_TEST_SANITIZE=address (or =thread); `make test-asan`
# wires this up. See firmware/daisy/tests/CMakeLists.txt for the rationale.
set(WAVEX_TEST_SANITIZE "" CACHE STRING "Sanitizer for host tests: address, thread, or empty")

if(WAVEX_TEST_SANITIZE)
    add_compile_options(-fsanitize=${WAVEX_TEST_SANITIZE} -fno-omit-frame-pointer
                        -fno-sanitize-recover=all -g -O1)
    add_link_options(-fsanitize=${WAVEX_TEST_SANITIZE})
    if(WAVEX_TEST_SANITIZE STREQUAL "address")
        add_compile_options(-fsanitize=undefined)
        add_link_options(-fsanitize=undefined)
    endif()
endif()

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

# Warning flags for FIRST-PARTY test code only - placed after
# FetchContent_MakeAvailable(googletest) so GoogleTest's own sources, which do
# not build clean under -Wconversion, are not affected.
#
# -Wconversion is the point of this block. 1d16237 was a size_t -> uint16_t
# narrowing that silently truncated a length and returned success; the
# compiler could have said so. Kept as warnings rather than -Werror: CI
# surfaces them, and a hard failure on a toolchain bump is worse than a
# visible warning.
add_compile_options(-Wall -Wextra -Wconversion)

# The ESP-IDF/FreeRTOS mocks must match vendor signatures exactly, so most of
# their parameters are unused by construction - that is what a stub is. Only
# -Wunused-parameter is suppressed, and only here: -Wconversion still applies,
# because a narrowing bug in a mock produces a wrong test result.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/mocks/esp32_mocks.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter"
)

# Ensure GoogleTest targets use C++17
if(TARGET gtest)
    set_target_properties(gtest PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
    )
endif()
if(TARGET gtest_main)
    set_target_properties(gtest_main PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
    )
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks  # Must be first to override lvgl.h, ui_theme.h, esp_lcd_types.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/links
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/components  # For file_browser.h, envelope_cache.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/styles  # For ui_theme.h (will use mock)
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/config  # For link_config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/spi_protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/uart_protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/freertos  # For FreeRTOS.h mock
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

# Test library with ESP32 source files (excluding problematic files)
# Exclude shared_packet_handler.cpp as it uses functions not in ProtocolHandler
# Exclude inter_mcu.cpp as it has external dependencies (uart_link, UI functions)
#
# packet_router.cpp's message handlers are declared weak under
# WAVEX_TEST_BUILD, so a test TU may provide strong overrides for the handlers
# it wants to observe; tests that don't override them get the real handlers,
# which report into the inter_mcu capture mocks. This is why a single library
# serves every test executable.
#
# wavex_application.cpp calls three functions whose headers (pcnt_task.h,
# ui_task.h) only declare them on-target; the forced include supplies host
# declarations, and esp32_mocks.cpp supplies configurable definitions.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../main/wavex_application.cpp
    PROPERTIES COMPILE_OPTIONS
    "-include;${CMAKE_CURRENT_SOURCE_DIR}/mocks/wavex_application_host_decls.h"
)

add_library(wavex_esp32_test_lib STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/application_context.cpp  # Include for testing
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/wavex_application.cpp  # Application lifecycle
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/packet_router.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/statistics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/comm_interface_impl.cpp  # Include for testing
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/components/envelope_cache.cpp  # Waveform envelope cache
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/components/envelope_fetcher.cpp  # Envelope request/receive state machine
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/components/file_browser.cpp  # File browser navigation logic
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/panel_key.cpp  # Panel key names and keycode map
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/uart_protocol/uart_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/spi_protocol/protocol.cpp  # Add protocol.cpp for ProtocolHandler
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/esp32_mocks.cpp  # Include mock implementations
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ui_mocks.cpp  # Include UI mock implementations
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/inter_mcu_mocks.cpp  # Mock inter-mcu linkage
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/test_helpers.cpp  # Include test helpers
)

target_include_directories(wavex_esp32_test_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/config
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks  # For esp_err.h, esp_timer.h mocks
)

target_compile_definitions(wavex_esp32_test_lib PUBLIC
    UNIT_TEST
    ESP_PLATFORM
    WAVEX_TEST_BUILD
)

target_link_libraries(wavex_esp32_test_lib PUBLIC
    Threads::Threads
    GTest::gtest
)

enable_testing()

# Function to add a test executable
function(add_wavex_esp32_test test_source)
    cmake_path(GET test_source STEM test_name)

    add_executable(${test_name} ${test_source})

    target_link_libraries(${test_name} PUBLIC
        wavex_esp32_test_lib
        Threads::Threads
        GTest::gtest_main
    )

    target_compile_definitions(${test_name} PRIVATE
        UNIT_TEST
        ESP_PLATFORM
        WAVEX_TEST_BUILD
    )

    set_target_properties(${test_name} PROPERTIES
        CXX_STANDARD 17
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

foreach(test_file IN LISTS unit_tests integration_tests)
    add_wavex_esp32_test(${test_file})
endforeach()

# Widget tests live in their own directory because they must NOT inherit the
# include_directories() above - `mocks/` is deliberately first there to shadow
# lvgl.h, and these tests need the real one. See widget/CMakeLists.txt.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/widget/CMakeLists.txt)
    add_subdirectory(widget)
endif()
