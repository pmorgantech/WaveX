# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add the component directories
set(EXTRA_COMPONENT_DIRS
    "components"
    "../shared"
)

# Exclude unnecessary components to reduce build size and prevent compilation errors.
#
# Only entries that actually take effect belong here. EXCLUDE_COMPONENTS cannot
# drop a component that something still depends on - IDF pulls it back in - so a
# listed-but-required name is not a smaller image, just a misleading file.
# Twelve former entries were removed after checking build/project_description.json
# and finding them in the built set regardless: esp_wifi, wpa_supplicant,
# esp_netif, esp_netif_stack, esp_mm, esp_gdbstub, wear_levelling, fatfs, spiffs,
# usb, vfs and unity. Two of those were self-contradictory as well - main/
# REQUIRES esp_mm, and sdkconfig sets CONFIG_ESP_GDBSTUB_ENABLED=y.
#
# Re-check with project_description.json before adding to this list.
set(EXCLUDE_COMPONENTS
    "esp_http_server"
    "esp_http_client"
    "esp_https_ota"
    "esp_https_server"
    "wifi_provisioning"
    "esp_bt"
    "esp_hid"
    "esp_eth"
    "esp_local_ctrl"
    "espcoredump"
    "mqtt"
    "protocomm"
    "protobuf-c"
    "touch_element"
    "idf_test"
    "openthread"
    "ieee802154"
    "perfmon"
    "ulp"
    "xtensa"
)

# Firmware version comes from the repo-root VERSION file, shared with the
# Daisy build so frontend and backend stay in sync. Bump that one file, not this one.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" PROJECT_VER)
# file(STRINGS) is not a dependency: without this an existing build dir keeps
# the version it was configured with and a bump ships in the old image.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../../VERSION")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Debug/release build profile (README.md#build-profiles). Master
# switch for the debug feature surface: logging_config.h defaults
# WAVEX_DEBUG_LOGGING_ENABLED, WAVEX_DEBUG_HARNESS_ENABLED and thus
# WAVEX_ESP_SCREENSHOT_DEBUG from it, so
# `idf.py -B build-release -DWAVEX_BUILD_DEBUG=OFF build` drops the console
# listener, the screenshot path and the command surface from the image.
#
# The placement is load-bearing and the window is exactly one statement wide.
# Earlier than the include() above fails with "Unknown CMake command
# idf_build_set_property" - that command is defined BY project.cmake. Later
# than the project() below applies to nothing, because ESP-IDF configures every
# component target inside that call; see the note under project() for the
# previous time this bit us.
option(WAVEX_BUILD_DEBUG "Debug profile: logging, console harness, screenshots" ON)
if(WAVEX_BUILD_DEBUG)
    set(WAVEX_BUILD_DEBUG_VALUE 1)
else()
    set(WAVEX_BUILD_DEBUG_VALUE 0)
endif()
idf_build_set_property(COMPILE_DEFINITIONS "WAVEX_BUILD_DEBUG=${WAVEX_BUILD_DEBUG_VALUE}" APPEND)

project(wavex-esp32)

# NOTE: do not add add_compile_options()/add_link_options() here. ESP-IDF
# configures every component target inside project() above, so anything added
# after it applies to nothing. This file used to carry
# `-Os -flto -ffunction-sections -fdata-sections` at this point, which read as
# a size-optimised LTO build while the image was in fact built -O2 with no LTO
# (CONFIG_COMPILER_OPTIMIZATION_PERF, confirmed in build/compile_commands.json).
# Optimisation level belongs in sdkconfig; per-component flags belong in that
# component's CMakeLists via target_compile_options().
