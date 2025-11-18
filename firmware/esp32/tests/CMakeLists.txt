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

# Always download GoogleTest via FetchContent to avoid path issues
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG release-1.12.1
)
# Prevent GoogleTest from overriding our C++ standard
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/components  # For file_browser.h
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
# Exclude wavex_application.cpp as it has many ESP-IDF dependencies
add_library(wavex_esp32_test_lib STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/application_context.cpp  # Include for testing
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/packet_router.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/statistics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/comm_interface_impl.cpp  # Include for testing
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/components/file_browser.cpp  # File browser navigation logic
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/uart_protocol/uart_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/spi_protocol/protocol.cpp  # Add protocol.cpp for ProtocolHandler
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/esp32_mocks.cpp  # Include mock implementations
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ui_mocks.cpp  # Include UI mock implementations
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

# Separate test library for packet_router_test with mock handlers
add_library(wavex_esp32_test_lib_no_router STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/application_context.cpp  # Include for testing
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/packet_router.cpp   # Include packet_router but with weak handlers
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/statistics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/comm_interface_impl.cpp  # Include for testing
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/components/file_browser.cpp  # File browser navigation logic
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/uart_protocol/uart_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/spi_protocol/protocol.cpp  # Add protocol.cpp for ProtocolHandler
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/esp32_mocks.cpp  # Include mock implementations
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ui_mocks.cpp  # Include UI mock implementations
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/test_helpers.cpp  # Include test helpers
)

target_include_directories(wavex_esp32_test_lib_no_router PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/config
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks  # For esp_err.h, esp_timer.h mocks
)

target_compile_definitions(wavex_esp32_test_lib_no_router PUBLIC
    UNIT_TEST
    ESP_PLATFORM
    WAVEX_TEST_BUILD
)

target_link_libraries(wavex_esp32_test_lib_no_router PUBLIC
    Threads::Threads
    GTest::gtest
)

enable_testing()

# Function to add a test executable
function(add_wavex_esp32_test test_source)
    cmake_path(GET test_source STEM test_name)

    add_executable(${test_name} ${test_source})

    # Use different library for packet_router_test to avoid linking real handlers
    if(${test_name} STREQUAL "packet_router_test")
        set(TEST_LIB wavex_esp32_test_lib_no_router)
    else()
        set(TEST_LIB wavex_esp32_test_lib)
    endif()

    target_link_libraries(${test_name} PUBLIC
        ${TEST_LIB}
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

# Exclude WaveXApplication test as it has many ESP-IDF dependencies
list(FILTER unit_tests EXCLUDE REGEX ".*wavex_application_test\.cpp$")

foreach(test_file IN LISTS unit_tests integration_tests)
    add_wavex_esp32_test(${test_file})
endforeach()
