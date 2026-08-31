cmake_minimum_required(VERSION 3.16)
project(wavex-shared-test)

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
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/_deps/googletest-src
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../spi_protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/../uart_protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/../wav
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

# Test library with shared protocol source files plus the test helpers
# (utils/test_helpers.cpp). The helpers were previously never compiled, so
# ProtocolTestHelper / CRCTestVectors silently rotted as dead code; they are
# now linked into every test binary and exercised by the protocol tests.
add_library(wavex_shared_test_lib STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../spi_protocol/protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../uart_protocol/uart_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/test_helpers.cpp
)

target_include_directories(wavex_shared_test_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_definitions(wavex_shared_test_lib PUBLIC
    UNIT_TEST
)

target_link_libraries(wavex_shared_test_lib PUBLIC
    Threads::Threads
)

set_target_properties(wavex_shared_test_lib PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
)

enable_testing()

# Function to add a test executable
function(add_wavex_shared_test test_source)
    cmake_path(GET test_source STEM test_name)

    add_executable(${test_name} ${test_source})

    target_link_libraries(${test_name} PUBLIC
        wavex_shared_test_lib
        Threads::Threads
        GTest::gtest
        GTest::gtest_main
    )

    target_compile_definitions(${test_name} PRIVATE
        UNIT_TEST
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
file(GLOB_RECURSE protocol_tests "${CMAKE_CURRENT_SOURCE_DIR}/protocol/*_test.cpp")
file(GLOB_RECURSE integration_tests "${CMAKE_CURRENT_SOURCE_DIR}/integration/*_test.cpp")
file(GLOB_RECURSE midi_tests "${CMAKE_CURRENT_SOURCE_DIR}/midi/*_test.cpp")
file(GLOB_RECURSE wav_tests "${CMAKE_CURRENT_SOURCE_DIR}/wav/*_test.cpp")
file(GLOB_RECURSE wxcf_tests "${CMAKE_CURRENT_SOURCE_DIR}/wxcf/*_test.cpp")
file(GLOB_RECURSE config_tests "${CMAKE_CURRENT_SOURCE_DIR}/config/*_test.cpp")

foreach(test_file IN LISTS protocol_tests integration_tests midi_tests wav_tests wxcf_tests config_tests)
    add_wavex_shared_test(${test_file})
endforeach()
