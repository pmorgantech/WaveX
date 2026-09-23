add_executable(usb_midi_test usb_midi_test.cpp
    ../../main/usb_midi_host.cpp ../../main/usb_midi_port.cpp)
target_include_directories(usb_midi_test BEFORE PRIVATE mocks)
target_compile_definitions(usb_midi_test PRIVATE WAVEX_TEST_BUILD)
target_compile_options(usb_midi_test PRIVATE
    -include ${CMAKE_CURRENT_SOURCE_DIR}/../mocks/panel_lock_host.h)
target_link_libraries(usb_midi_test PRIVATE gtest_main Threads::Threads)
gtest_discover_tests(usb_midi_test)
