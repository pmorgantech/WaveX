/**
 * @file usb_midi_task.cpp
 * @brief USB MIDI device input task (roadmap Phase 1 item 8)
 *
 * Descriptor layout follows the ESP-IDF tusb_midi example (esp_tinyusb
 * >= 2.0): one MIDI interface pair (control + streaming) on endpoint 1,
 * default Espressif device descriptor, WaveX strings. The P4's USB-OTG
 * is high-speed-capable, so both FS and HS configuration descriptors are
 * provided and TinyUSB picks by negotiated speed.
 *
 * Latency: no polling - TinyUSB's device task invokes tud_midi_rx_cb()
 * on reception (task context), which notifies the reader task; the
 * reader drains tud_midi_stream_read() (the class driver's re-assembled
 * plain MIDI byte stream) through the shared StreamParser and forwards
 * notes via midi_forward_event(). Same < 5 ms in-to-sound shape as DIN,
 * minus the 31250-baud wire time.
 */

#include "usb_midi_task.h"

#include "config/hardware_config.h"

#if WAVEX_ESP_USB_MIDI_ENABLED && WAVEX_USB_MIDI_INPUT_ENABLED

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "midi_task.h"  // midi_forward_event()
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char* TAG = "usb_midi";

static TaskHandle_t s_usb_midi_task_handle = nullptr;
static bool s_driver_installed = false;

// --- TinyUSB descriptors (tusb_midi example layout) ---

enum {  // interfaces: MIDI is a control + streaming interface pair
    ITF_NUM_MIDI = 0,
    ITF_NUM_MIDI_STREAMING,
    ITF_COUNT
};

enum {  // endpoint numbers (0 is reserved)
    EP_EMPTY = 0,
    EPNUM_MIDI,
};

#define TUSB_DESCRIPTOR_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)

static const char* s_str_desc[] = {
    (const char[]){0x09, 0x04},  // 0: language = English (0x0409)
    "WaveX",                     // 1: manufacturer
    "WaveX Sampler",             // 2: product
    "0001",                      // 3: serial
    "WaveX MIDI",                // 4: MIDI interface name
};

static const uint8_t s_midi_fs_cfg_desc[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, TUSB_DESCRIPTOR_TOTAL_LEN, 0, 100),
    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 4, EPNUM_MIDI, (0x80 | EPNUM_MIDI), 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t s_midi_hs_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, TUSB_DESCRIPTOR_TOTAL_LEN, 0, 100),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 4, EPNUM_MIDI, (0x80 | EPNUM_MIDI), 512),
};
#endif  // TUD_OPT_HIGH_SPEED

// --- reader ---

static void usb_midi_task(void* arg) {
    (void)arg;
    WaveX::Midi::StreamParser parser;
    WaveX::Midi::Event ev;
    uint8_t buf[64];

    ESP_LOGI(TAG, "USB MIDI reader running");

    for (;;) {
        // Woken by tud_midi_rx_cb() below on every received packet; the
        // read must fully drain regardless (the MIDI interface always has
        // an OUT endpoint, and unread data would stall the host side).
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t n;
        while ((n = tud_midi_stream_read(buf, sizeof(buf))) > 0) {
            for (uint32_t i = 0; i < n; ++i) {
                if (parser.Feed(buf[i], ev)) {
                    midi_forward_event(ev);
                }
            }
        }
    }
}

// TinyUSB weak-symbol override: called from the TinyUSB device task (not
// ISR) whenever MIDI data arrives, so the plain task-notify API is safe.
extern "C" void tud_midi_rx_cb(uint8_t itf) {
    (void)itf;
    if (s_usb_midi_task_handle) {
        xTaskNotifyGive(s_usb_midi_task_handle);
    }
}

extern "C" esp_err_t usb_midi_task_start(void) {
    if (s_usb_midi_task_handle) {
        return ESP_OK;  // already running
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.string = s_str_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_str_desc) / sizeof(s_str_desc[0]);
    tusb_cfg.descriptor.full_speed_config = s_midi_fs_cfg_desc;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_midi_hs_cfg_desc;
    tusb_cfg.descriptor.qualifier = NULL;
#endif  // TUD_OPT_HIGH_SPEED

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    s_driver_installed = true;

    BaseType_t rc = xTaskCreate(usb_midi_task,
                                "usb_midi",
                                WAVEX_USB_MIDI_TASK_STACK_SIZE,
                                nullptr,
                                WAVEX_USB_MIDI_TASK_PRIORITY,
                                &s_usb_midi_task_handle);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_usb_midi_task_handle = nullptr;
        usb_midi_task_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

extern "C" esp_err_t usb_midi_task_stop(void) {
    if (s_usb_midi_task_handle) {
        vTaskDelete(s_usb_midi_task_handle);
        s_usb_midi_task_handle = nullptr;
    }
    if (s_driver_installed) {
        tinyusb_driver_uninstall();
        s_driver_installed = false;
    }
    return ESP_OK;
}

#else  // !(WAVEX_ESP_USB_MIDI_ENABLED && WAVEX_USB_MIDI_INPUT_ENABLED)

extern "C" esp_err_t usb_midi_task_start(void) {
    return ESP_OK;
}

extern "C" esp_err_t usb_midi_task_stop(void) {
    return ESP_OK;
}

#endif  // WAVEX_ESP_USB_MIDI_ENABLED && WAVEX_USB_MIDI_INPUT_ENABLED
