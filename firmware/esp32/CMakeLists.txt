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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wavex-esp32)

# NOTE: do not add add_compile_options()/add_link_options() here. ESP-IDF
# configures every component target inside project() above, so anything added
# after it applies to nothing. This file used to carry
# `-Os -flto -ffunction-sections -fdata-sections` at this point, which read as
# a size-optimised LTO build while the image was in fact built -O2 with no LTO
# (CONFIG_COMPILER_OPTIMIZATION_PERF, confirmed in build/compile_commands.json).
# Optimisation level belongs in sdkconfig; per-component flags belong in that
# component's CMakeLists via target_compile_options().
