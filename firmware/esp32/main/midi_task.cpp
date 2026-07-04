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

#if WAVEX_ESP_DIN_MIDI_ENABLED

#include "config/pin_config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inter_mcu.h"

#include "midi/midi_stream_parser.hpp"

static const char* TAG = "midi_task";

static TaskHandle_t s_midi_task_handle = nullptr;
static bool s_driver_installed = false;

static void handle_event(const WaveX::Midi::Event& ev) {
    switch (ev.type) {
        case WaveX::Midi::EventType::NoteOn: {
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

static void midi_task(void* arg) {
    (void)arg;
    WaveX::Midi::StreamParser parser;
    WaveX::Midi::Event ev;
    uint8_t buf[64];

    ESP_LOGI(TAG,
             "DIN MIDI reader running (UART%d, %d baud)",
             (int)WAVEX_ESP_MIDI_UART_NUM,
             (int)WAVEX_ESP_MIDI_BAUD);

    for (;;) {
        // Block until at least one byte arrives, then drain the backlog
        // without blocking so bursts (chords, running-status streams) are
        // processed in one pass.
        int n = uart_read_bytes(WAVEX_ESP_MIDI_UART_NUM, buf, 1, portMAX_DELAY);
        while (n > 0) {
            for (int i = 0; i < n; ++i) {
                if (parser.Feed(buf[i], ev)) {
                    handle_event(ev);
                }
            }
            n = uart_read_bytes(WAVEX_ESP_MIDI_UART_NUM, buf, sizeof(buf), 0);
        }
    }
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

    BaseType_t rc = xTaskCreate(midi_task,
                                "din_midi",
                                WAVEX_DIN_MIDI_TASK_STACK_SIZE,
                                nullptr,
                                WAVEX_DIN_MIDI_TASK_PRIORITY,
                                &s_midi_task_handle);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_midi_task_handle = nullptr;
        midi_task_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

extern "C" esp_err_t midi_task_stop(void) {
    if (s_midi_task_handle) {
        vTaskDelete(s_midi_task_handle);
        s_midi_task_handle = nullptr;
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
