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
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

# Test library with shared protocol source files
add_library(wavex_shared_test_lib STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../spi_protocol/protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../uart_protocol/uart_protocol.cpp
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

foreach(test_file IN LISTS protocol_tests integration_tests midi_tests)
    add_wavex_shared_test(${test_file})
endforeach()
