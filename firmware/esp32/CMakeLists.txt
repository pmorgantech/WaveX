# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add the component directories
set(EXTRA_COMPONENT_DIRS 
    "components"
    "../shared"
)

# Exclude unnecessary components to reduce build size and prevent compilation errors
set(EXCLUDE_COMPONENTS 
    "esp_http_server"
    "esp_http_client" 
    "esp_https_ota"
    "esp_https_server"
    "esp_wifi"
    "wifi_provisioning"
    "wpa_supplicant"
    "esp_bt"
    "esp_hid"
    "esp_eth"
    "esp_netif"
    "esp_netif_stack"
    "esp_local_ctrl"
    "esp_mm"
    "esp_gdbstub"
    "espcoredump"
    "mqtt"
    "protocomm"
    "protobuf-c"
    "touch_element"
    "wear_levelling"
    "fatfs"
    "spiffs"
    "usb"
    "vfs"
    "unity"
    "idf_test"
    "openthread"
    "ieee802154"
    "perfmon"
    "ulp"
    "xtensa"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wavex-esp32) 