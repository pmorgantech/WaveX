/**
 * @file usb_midi_task.cpp
 * @brief USB MIDI device I/O (roadmap Phase 2.P.5)
 *
 * Descriptor layout follows the ESP-IDF tusb_midi example (esp_tinyusb
 * >= 2.0): one MIDI interface pair (control + streaming) on endpoint 1,
 * default Espressif device descriptor, WaveX strings. The P4's USB-OTG
 * is high-speed-capable, so both FS and HS configuration descriptors are
 * provided and TinyUSB picks by negotiated speed.
 *
 * RX callbacks wake the I/O task through a permanent semaphore. A bounded
 * service pass drains input and serializes queued clock/transport messages.
 * Physical latency and jitter remain a hardware validation gate (HV-014).
 */

#include "usb_midi_task.h"

#include "config/hardware_config.h"
#include "midi_out.h"

#if WAVEX_ESP_USB_MIDI_ENABLED && (WAVEX_USB_MIDI_INPUT_ENABLED || WAVEX_USB_MIDI_OUTPUT_ENABLED)

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "midi_task.h"  // midi_forward_event()
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include <atomic>
#include <cstdio>

static const char* TAG = "usb_midi";

// Lifecycle calls are serialized by the application. The callback signals a
// permanent semaphore, never a task handle that shutdown could free.
static std::atomic<TaskHandle_t> s_usb_midi_task_handle{nullptr};
static std::atomic<bool> s_usb_midi_running{false};
static bool s_driver_installed = false;
static StaticSemaphore_t s_rx_signal_storage;
static SemaphoreHandle_t s_rx_signal = nullptr;

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

// Per-unit serial, derived from the factory MAC. A hardcoded string means two
// WaveX units on one host present the same serial, and DAWs key their saved
// port assignments on it - so the second unit silently inherits the first's
// routing.
static char s_serial[13] = "000000000000";

static void init_serial_from_mac() {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) != ESP_OK &&
        esp_efuse_mac_get_default(mac) != ESP_OK) {
        return;  // keep the placeholder; a wrong serial beats no enumeration
    }
    snprintf(s_serial,
             sizeof(s_serial),
             "%02X%02X%02X%02X%02X%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

static const char* s_str_desc[] = {
    (const char[]){0x09, 0x04},  // 0: language = English (0x0409)
    "WaveX",                     // 1: manufacturer
    "WaveX Sampler",             // 2: product
    s_serial,                    // 3: serial, from the eFuse MAC (see below)
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
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // publish handle before any exit
    WaveX::Midi::StreamParser parser;
    WaveX::Midi::Event ev;
    uint8_t buf[64];
    bool connected_before = false;
    ESP_LOGI(TAG, "USB MIDI I/O running");
    while (s_usb_midi_running) {
        const bool connected = tud_mounted() && !tud_suspended();
        if (connected != connected_before)
            parser.Reset();
        connected_before = connected;
        wavex_midi::Ready(wavex_midi::Port::Usb, connected && WAVEX_USB_MIDI_OUTPUT_ENABLED);
        // Always drain OUT, even in output-only builds. Bound each pass so
        // host floods cannot starve output or the shutdown handshake.
        const auto count = tud_midi_stream_read(buf, sizeof(buf));
#if WAVEX_USB_MIDI_INPUT_ENABLED
        for (uint32_t i = 0; i < count; ++i)
            if (connected && parser.Feed(buf[i], ev))
                midi_forward_event(ev);
#else
        (void)count;
        (void)ev;
#endif
#if WAVEX_USB_MIDI_OUTPUT_ENABLED
        WaveX::Midi::ClockPacket packet;
        if (connected && wavex_midi::Take(wavex_midi::Port::Usb, packet)) {
            const auto usb = packet.Usb();
            // Packet API accepts all four bytes or none. A full endpoint drops
            // this event with a counter; never replay clock bursts later.
            wavex_midi::Complete(wavex_midi::Port::Usb, tud_midi_packet_write(usb.data()));
        }
#endif
        xSemaphoreTake(s_rx_signal, pdMS_TO_TICKS(1));
    }
    wavex_midi::Ready(wavex_midi::Port::Usb, false);
    s_usb_midi_task_handle = nullptr;
    vTaskDelete(nullptr);
}

extern "C" void tud_midi_rx_cb(uint8_t itf) {
    (void)itf;
    // Installed only after the permanent semaphore has been initialized.
    xSemaphoreGive(s_rx_signal);
}

extern "C" esp_err_t usb_midi_task_start(void) {
    if (s_usb_midi_task_handle) {
        return ESP_OK;  // already running
    }
    if (s_driver_installed)
        return ESP_ERR_INVALID_STATE;

    if (!s_rx_signal)
        s_rx_signal = xSemaphoreCreateBinaryStatic(&s_rx_signal_storage);
    if (!s_rx_signal)
        return ESP_ERR_NO_MEM;
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.string = s_str_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_str_desc) / sizeof(s_str_desc[0]);
    tusb_cfg.descriptor.full_speed_config = s_midi_fs_cfg_desc;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_midi_hs_cfg_desc;
    tusb_cfg.descriptor.qualifier = NULL;
#endif  // TUD_OPT_HIGH_SPEED

    init_serial_from_mac();

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    s_driver_installed = true;

    s_usb_midi_running = true;
    TaskHandle_t handle = nullptr;
    BaseType_t rc = xTaskCreate(usb_midi_task,
                                "usb_midi",
                                WAVEX_USB_MIDI_TASK_STACK_SIZE,
                                nullptr,
                                WAVEX_USB_MIDI_TASK_PRIORITY,
                                &handle);
    s_usb_midi_task_handle = handle;
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_usb_midi_task_handle = nullptr;
        s_usb_midi_running = false;
        usb_midi_task_stop();
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(handle);
    return ESP_OK;
}

extern "C" esp_err_t usb_midi_task_stop(void) {
    s_usb_midi_running = false;
    wavex_midi::Ready(wavex_midi::Port::Usb, false);
    if (s_rx_signal)
        xSemaphoreGive(s_rx_signal);
    // Wait before uninstalling TinyUSB: the worker may still be in its API.
    for (int waited_ms = 0; s_usb_midi_task_handle && waited_ms < 300; waited_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_usb_midi_task_handle) {
        ESP_LOGE(TAG, "USB MIDI task did not exit; leaving the TinyUSB driver installed");
        return ESP_ERR_TIMEOUT;
    }
    if (s_driver_installed) {
        const auto result = tinyusb_driver_uninstall();
        if (result != ESP_OK)
            return result;
        s_driver_installed = false;
    }
    return ESP_OK;
}

#else  // USB MIDI disabled

extern "C" esp_err_t usb_midi_task_start(void) {
    return ESP_OK;
}

extern "C" esp_err_t usb_midi_task_stop(void) {
    return ESP_OK;
}

#endif  // USB MIDI input or output
