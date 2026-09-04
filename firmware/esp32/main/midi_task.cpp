/**
 * @file midi_task.cpp
 * @brief DIN MIDI input task (roadmap Phase 1 item 8)
 *
 * Latency shape (item 8 budget: < 5 ms MIDI-in to sound): a 3-byte note
 * message takes ~960 us on the wire at 31250 baud. The UART RX-full
 * threshold is set to 1 and the RX timeout to 1 symbol so the driver ISR
 * posts bytes to the ring as they arrive instead of batching them; the
 * task blocks on the first byte and then drains whatever else is pending
 * with a zero timeout. Interrupt load is bounded by the MIDI wire itself
 * (<= 3125 bytes/s). Forwarding is one small frame over the 2 Mbaud
 * inter-MCU link (~100 us wire time), and the Daisy applies the note at
 * the next 1 ms audio block.
 */

#include "midi_task.h"

#include "config/hardware_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "inter_mcu.h"

#include <atomic>

static const char* TAG = "midi_task";

// Receive-channel filter. 0 = Omni, 1..16 = that MIDI channel (the wire
// channel is 0-based, so the comparison subtracts one). Written by the UI
// task from Settings > MIDI, read on the DIN and USB reader tasks - hence
// an atomic. Relaxed is enough: it guards nothing but itself, and a note
// either side of the change is equally correct.
//
// Not persisted. The frontend has no NVS code at all today, and inventing a
// store for one integer is a bigger decision than this page should make; the
// setting reads Omni again after a reboot, and the page says so.
static std::atomic<int> s_input_channel{0};

void midi_set_input_channel(int channel) {
    if (channel < 0 || channel > 16) {
        return;
    }
    s_input_channel.store(channel, std::memory_order_relaxed);
    ESP_LOGI(TAG, "MIDI input channel filter: %s", channel == 0 ? "Omni" : "single");
}

int midi_get_input_channel(void) {
    return s_input_channel.load(std::memory_order_relaxed);
}

// Shared with the USB MIDI reader (usb_midi_task.cpp) - declared in
// midi_task.h. Not gated on WAVEX_ESP_DIN_MIDI_ENABLED so either
// transport can be compiled out independently.
void midi_forward_event(const WaveX::Midi::Event& ev) {
    switch (ev.type) {
        case WaveX::Midi::EventType::NoteOn: {
            // Note On is the only filtered message; see midi_task.h for why
            // Note Off is not.
            const int filter = s_input_channel.load(std::memory_order_relaxed);
            if (filter != 0 && ev.channel != static_cast<uint8_t>(filter - 1)) {
                break;
            }
            esp_err_t err = inter_mcu_send_note_on(ev.data1, ev.data2, ev.channel);
            if (err != ESP_OK) {
                ESP_LOGW(
                    TAG, "note-on %u dropped (link send failed: %d)", (unsigned)ev.data1, (int)err);
            }
            break;
        }
        case WaveX::Midi::EventType::NoteOff: {
            esp_err_t err = inter_mcu_send_note_off(ev.data1, ev.channel);
            if (err != ESP_OK) {
                ESP_LOGW(TAG,
                         "note-off %u dropped (link send failed: %d)",
                         (unsigned)ev.data1,
                         (int)err);
            }
            break;
        }
        case WaveX::Midi::EventType::ControlChange:
            // Not forwarded yet: MSG_CONTROL_CHANGE carries WaveX PARAM_*
            // ids, not raw MIDI CC numbers - a CC-to-parameter mapping
            // policy is Phase 2 territory (front-panel/sequencer work).
            break;
    }
}

#if WAVEX_ESP_DIN_MIDI_ENABLED

#include "config/pin_config.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static std::atomic<TaskHandle_t> s_midi_task_handle{nullptr};
static bool s_driver_installed = false;
// Shutdown handshake. Deleting a task that is blocked inside the UART driver
// and then deleting that driver is undefined behaviour, so stop() asks the
// task to leave and waits for it to say it has.
static std::atomic<bool> s_midi_running{false};

static void midi_task(void* arg) {
    (void)arg;
    WaveX::Midi::StreamParser parser;
    WaveX::Midi::Event ev;
    uint8_t buf[64];

    ESP_LOGI(TAG,
             "DIN MIDI reader running (UART%d, %d baud)",
             (int)WAVEX_ESP_MIDI_UART_NUM,
             (int)WAVEX_ESP_MIDI_BAUD);

    // Storm detector. Real MIDI cannot exceed 3125 bytes/s, i.e. ~1000
    // running-status note events per second at the theoretical limit, and a
    // player produces a few tens. A sustained rate above kStormEventsPerSec
    // is not music: it is a floating or shared input being parsed (seen
    // 2026-09-04 at ~700 events/s - USB D- traffic on GPIO24 read as MIDI,
    // note numbers 0/4/8/32/64), and it loads the inter-MCU link. Warn once
    // per storm rather than per event.
    constexpr uint32_t kStormEventsPerSec = 500;
    uint32_t events_this_window = 0;
    int64_t window_start_us = esp_timer_get_time();
    bool storm_reported = false;

    while (s_midi_running) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us - window_start_us >= 1000000) {
            if (events_this_window > kStormEventsPerSec && !storm_reported) {
                ESP_LOGW(TAG,
                         "DIN MIDI input storm: %u events/s - check the MIDI-in wiring "
                         "(floating RX reads as notes)",
                         (unsigned)events_this_window);
                storm_reported = true;
            } else if (events_this_window <= kStormEventsPerSec && storm_reported) {
                ESP_LOGI(
                    TAG, "DIN MIDI input storm over (%u events/s)", (unsigned)events_this_window);
                storm_reported = false;
            }
            events_this_window = 0;
            window_start_us = now_us;
        }
        // Block for one byte, then drain the backlog without blocking so
        // bursts (chords, running-status streams) are processed in one pass.
        // The wait is bounded rather than portMAX_DELAY purely so the loop
        // notices a stop request; an idle wakeup every 100 ms costs nothing
        // next to being unable to shut down without undefined behaviour.
        int n = uart_read_bytes(WAVEX_ESP_MIDI_UART_NUM, buf, 1, pdMS_TO_TICKS(100));
        while (n > 0) {
            for (int i = 0; i < n; ++i) {
                if (parser.Feed(buf[i], ev)) {
                    ++events_this_window;
                    midi_forward_event(ev);
                }
            }
            n = uart_read_bytes(WAVEX_ESP_MIDI_UART_NUM, buf, sizeof(buf), 0);
        }
    }

    // Publish the exit before self-deleting: stop() waits on this, and only
    // then is it safe to delete the driver this task was reading from.
    s_midi_task_handle = nullptr;
    vTaskDelete(nullptr);
}

extern "C" esp_err_t midi_task_start(void) {
    if (s_midi_task_handle) {
        return ESP_OK;  // already running
    }

    uart_config_t cfg = {};
    cfg.baud_rate = WAVEX_ESP_MIDI_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    // RX-only ring (TX ring 0: MIDI out is not part of item 8; the TX pin
    // is still claimed in pin_config for later MIDI clock out, Phase 2).
    esp_err_t err =
        uart_driver_install(WAVEX_ESP_MIDI_UART_NUM, WAVEX_DIN_MIDI_RX_BUF_SIZE, 0, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    s_driver_installed = true;

    err = uart_param_config(WAVEX_ESP_MIDI_UART_NUM, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(WAVEX_ESP_MIDI_UART_NUM,
                           WAVEX_ESP_MIDI_TX,
                           WAVEX_ESP_MIDI_RX,
                           UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    // No pull on the RX pin. The obvious move for a floating UART input is
    // an internal pull-up, and it was tried on 2026-09-04: WAVEX_ESP_MIDI_RX
    // is GPIO24, which on the ESP32-P4 is also USB D- of the built-in
    // USB-Serial/JTAG PHY, and the pull-up took that port off the bus while
    // the app ran. That shared pin is the reason for the storm detector
    // below and for WAVEX_ESP_DIN_MIDI_ENABLED defaulting to 0 in
    // hardware_config.h until the input is re-pinned.
    // Per-byte delivery for latency: ISR fires on every RX byte (threshold
    // 1) and the idle timeout is 1 symbol, so nothing sits in the FIFO.
    if (err == ESP_OK) {
        err = uart_set_rx_full_threshold(WAVEX_ESP_MIDI_UART_NUM, 1);
    }
    if (err == ESP_OK) {
        err = uart_set_rx_timeout(WAVEX_ESP_MIDI_UART_NUM, 1);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed: %s", esp_err_to_name(err));
        midi_task_stop();
        return err;
    }

    s_midi_running = true;
    TaskHandle_t handle = nullptr;
    BaseType_t rc = xTaskCreate(midi_task,
                                "din_midi",
                                WAVEX_DIN_MIDI_TASK_STACK_SIZE,
                                nullptr,
                                WAVEX_DIN_MIDI_TASK_PRIORITY,
                                &handle);
    s_midi_task_handle = handle;
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_midi_task_handle = nullptr;
        s_midi_running = false;
        midi_task_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

extern "C" esp_err_t midi_task_stop(void) {
    s_midi_running = false;
    // Wait for the task to leave its loop and self-delete rather than killing
    // it: vTaskDelete() on a task blocked inside uart_read_bytes() leaves the
    // driver's internals inconsistent, and the uart_driver_delete() below then
    // frees objects it is still parked on.
    for (int waited_ms = 0; s_midi_task_handle && waited_ms < 300; waited_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_midi_task_handle) {
        // It did not leave. Deleting the driver now would be worse than
        // leaking the task, so keep the driver and say so.
        ESP_LOGE(TAG, "DIN MIDI task did not exit; leaving the UART driver installed");
        return ESP_ERR_TIMEOUT;
    }
    if (s_driver_installed) {
        uart_driver_delete(WAVEX_ESP_MIDI_UART_NUM);
        s_driver_installed = false;
    }
    return ESP_OK;
}

#else  // !WAVEX_ESP_DIN_MIDI_ENABLED

extern "C" esp_err_t midi_task_start(void) {
    return ESP_OK;
}

extern "C" esp_err_t midi_task_stop(void) {
    return ESP_OK;
}

#endif  // WAVEX_ESP_DIN_MIDI_ENABLED
