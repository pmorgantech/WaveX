#pragma once
#include "esp_err.h"

#include <cstddef>
#include <cstdint>
using usb_host_client_handle_t = void*;
using usb_device_handle_t = void*;
constexpr int ESP_INTR_FLAG_LEVEL1 = 1;
enum usb_host_client_event_t { USB_HOST_CLIENT_EVENT_NEW_DEV, USB_HOST_CLIENT_EVENT_DEV_GONE };
struct usb_host_client_event_msg_t {
    usb_host_client_event_t event;
    union {
        struct {
            uint8_t address;
        } new_dev;
        struct {
            usb_device_handle_t dev_hdl;
        } dev_gone;
    };
};
enum usb_transfer_status_t {
    USB_TRANSFER_STATUS_COMPLETED,
    USB_TRANSFER_STATUS_ERROR,
    USB_TRANSFER_STATUS_CANCELED,
    USB_TRANSFER_STATUS_NO_DEVICE
};
struct usb_transfer_t {
    uint8_t* data_buffer;
    size_t data_buffer_size;
    usb_device_handle_t device_handle;
    uint8_t bEndpointAddress;
    int num_bytes, actual_num_bytes;
    usb_transfer_status_t status;
    void (*callback)(usb_transfer_t*);
    void* context;
};
struct __attribute__((packed)) usb_config_desc_t {
    uint8_t bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t rest[5];
};
struct usb_host_config_t {
    bool root_port_unpowered;
    int intr_flags;
};
struct usb_host_client_config_t {
    int max_num_event_msg;
    struct {
        void (*client_event_callback)(const usb_host_client_event_msg_t*, void*);
        void* callback_arg;
    } async;
};
const char* esp_err_to_name(esp_err_t);
esp_err_t usb_host_install(const usb_host_config_t*);
esp_err_t usb_host_uninstall();
esp_err_t usb_host_client_register(const usb_host_client_config_t*, usb_host_client_handle_t*);
esp_err_t usb_host_client_deregister(usb_host_client_handle_t);
esp_err_t usb_host_transfer_alloc(size_t, int, usb_transfer_t**);
esp_err_t usb_host_transfer_free(usb_transfer_t*);
esp_err_t usb_host_transfer_submit(usb_transfer_t*);
esp_err_t usb_host_lib_set_root_port_power(bool);
esp_err_t usb_host_device_free_all();
esp_err_t usb_host_lib_handle_events(uint32_t, uint32_t*);
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t, uint32_t);
esp_err_t usb_host_device_open(usb_host_client_handle_t, uint8_t, usb_device_handle_t*);
esp_err_t usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t);
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t, const usb_config_desc_t**);
esp_err_t usb_host_interface_claim(usb_host_client_handle_t, usb_device_handle_t, uint8_t, uint8_t);
esp_err_t usb_host_interface_release(usb_host_client_handle_t, usb_device_handle_t, uint8_t);
esp_err_t usb_host_endpoint_halt(usb_device_handle_t, uint8_t);
esp_err_t usb_host_endpoint_flush(usb_device_handle_t, uint8_t);
