#include "ui/ui_screenshot.h"

#if WAVEX_ESP_SCREENSHOT_DEBUG

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <cstdio>
#include <cstring>

namespace {

const char* TAG = "UI_SCREENSHOT";
constexpr char kToken[] = "WAVEX-SCREENSHOT";
constexpr size_t kTokenLen = sizeof(kToken) - 1;

// Console UART. The trigger arrives on the same port the logs leave on, so
// the host script can drive everything through one tty.
constexpr uart_port_t kUart = UART_NUM_0;

enum class State : uint8_t {
    Idle,
    Requested,  // token seen; UI task should capture on next poll
    Captured,   // buffer ready; listener task prints and frees it
    Failed,
};

volatile State s_state = State::Idle;
bool s_started = false;

lv_draw_buf_t s_buf;
uint8_t* s_pixels = nullptr;  // PSRAM, aligned; valid in Captured state
uint32_t s_w = 0, s_h = 0, s_stride = 0;

// ---------------------------------------------------------------------------
// Dump path (listener task context; UI task is NOT blocked by this)
// ---------------------------------------------------------------------------

// RLE: (count u8 >= 1, pixel u16 LE) pairs, row-major over w*h logical pixels
// (stride padding excluded). Flat UI compresses 30-100x, which is what makes
// a 115200-baud dump take seconds instead of minutes.
size_t rle_encode(uint8_t* dst, size_t cap) {
    size_t out = 0;
    for (uint32_t y = 0; y < s_h; y++) {
        const uint16_t* row = reinterpret_cast<const uint16_t*>(s_pixels + y * s_stride);
        uint32_t x = 0;
        while (x < s_w) {
            uint16_t px = row[x];
            uint32_t run = 1;
            while (x + run < s_w && row[x + run] == px && run < 255) {
                run++;
            }
            if (out + 3 > cap) {
                return 0;  // cannot happen with the sizing below; hard fail if it does
            }
            dst[out++] = static_cast<uint8_t>(run);
            dst[out++] = static_cast<uint8_t>(px & 0xFF);
            dst[out++] = static_cast<uint8_t>(px >> 8);
            x += run;
        }
    }
    return out;
}

void base64_print(const uint8_t* data, size_t len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[89];  // 88 chars = 66 input bytes per printed line
    size_t li = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = data[i] << 16;
        int rem = static_cast<int>(len - i);
        if (rem > 1)
            v |= data[i + 1] << 8;
        if (rem > 2)
            v |= data[i + 2];
        line[li++] = tbl[(v >> 18) & 0x3F];
        line[li++] = tbl[(v >> 12) & 0x3F];
        line[li++] = (rem > 1) ? tbl[(v >> 6) & 0x3F] : '=';
        line[li++] = (rem > 2) ? tbl[v & 0x3F] : '=';
        if (li >= 88) {
            line[li] = '\0';
            printf("%s\n", line);
            li = 0;
            // Console TX is interrupt-driven but finite; a short yield keeps
            // the log task and watchdog fed during a multi-second dump.
            vTaskDelay(1);
        }
    }
    if (li) {
        line[li] = '\0';
        printf("%s\n", line);
    }
}

void dump_and_release() {
    // Worst-case RLE is 3 bytes per pixel (all runs of 1) - by construction
    // it cannot exceed that, so this allocation makes rle_encode total.
    const size_t cap = static_cast<size_t>(s_w) * s_h * 3;
    uint8_t* rle = static_cast<uint8_t*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (!rle) {
        printf("=== WAVEX SCREENSHOT ERROR rle-alloc ===\n");
        return;
    }
    const size_t rle_len = rle_encode(rle, cap);
    if (rle_len == 0) {
        heap_caps_free(rle);
        printf("=== WAVEX SCREENSHOT ERROR rle-overflow ===\n");
        return;
    }
    const uint32_t crc = esp_rom_crc32_le(0, rle, rle_len);
    printf("=== WAVEX SCREENSHOT BEGIN w=%lu h=%lu fmt=rgb565 enc=rle+b64 len=%u ===\n",
           static_cast<unsigned long>(s_w),
           static_cast<unsigned long>(s_h),
           static_cast<unsigned>(rle_len));
    base64_print(rle, rle_len);
    printf("=== WAVEX SCREENSHOT END crc=%08lx ===\n", static_cast<unsigned long>(crc));
    heap_caps_free(rle);
}

void listener_task(void*) {
    uint8_t buf[64];
    size_t matched = 0;
    for (;;) {
        int n = uart_read_bytes(kUart, buf, sizeof(buf), pdMS_TO_TICKS(200));
        for (int i = 0; i < n; i++) {
            const char c = static_cast<char>(buf[i]);
            if (c == kToken[matched]) {
                if (++matched == kTokenLen) {
                    matched = 0;
                    if (s_state == State::Idle) {
                        ESP_LOGI(TAG, "Screenshot requested");
                        s_state = State::Requested;
                    }
                }
            } else {
                matched = (c == kToken[0]) ? 1u : 0u;
            }
        }

        if (s_state == State::Captured) {
            dump_and_release();
            heap_caps_free(s_pixels);
            s_pixels = nullptr;
            s_state = State::Idle;
        } else if (s_state == State::Failed) {
            printf("=== WAVEX SCREENSHOT ERROR capture ===\n");
            s_state = State::Idle;
        }
    }
}

void ensure_started() {
    if (s_started) {
        return;
    }
    s_started = true;

    // RX-only driver on the console UART for the trigger token. Console TX
    // (logging) does not go through this driver, so output is unaffected.
    if (!uart_is_driver_installed(kUart)) {
        esp_err_t err = uart_driver_install(kUart, 512, 0, 0, nullptr, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "uart_driver_install failed (%s) - screenshots disabled",
                     esp_err_to_name(err));
            return;
        }
    }
    xTaskCreate(listener_task, "scrshot", 4096, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "Serial screenshot armed (token: %s)", kToken);
}

}  // namespace

void wavex_screenshot_poll() {
    ensure_started();
    if (s_state != State::Requested) {
        return;
    }

    // Capture under the LVGL lock, in the UI task - the only context allowed
    // to touch LVGL. The expensive part (the serial dump) happens in the
    // listener task afterwards, so the UI stalls only for the render
    // (tens of milliseconds), not for the seconds of printing.
    lvgl_port_lock(portMAX_DELAY);
    lv_obj_t* scr = lv_screen_active();
    s_w = static_cast<uint32_t>(lv_obj_get_width(scr));
    s_h = static_cast<uint32_t>(lv_obj_get_height(scr));
    s_stride = lv_draw_buf_width_to_stride(s_w, LV_COLOR_FORMAT_RGB565);

    const size_t size = static_cast<size_t>(s_stride) * s_h;
    s_pixels = static_cast<uint8_t*>(heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM));
    if (!s_pixels) {
        lvgl_port_unlock();
        ESP_LOGW(TAG, "PSRAM alloc failed (%u bytes)", static_cast<unsigned>(size));
        s_state = State::Failed;
        return;
    }
    lv_draw_buf_init(&s_buf, s_w, s_h, LV_COLOR_FORMAT_RGB565, s_stride, s_pixels, size);

    const lv_result_t res = lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &s_buf);
    lvgl_port_unlock();

    if (res != LV_RESULT_OK) {
        heap_caps_free(s_pixels);
        s_pixels = nullptr;
        ESP_LOGW(TAG, "lv_snapshot_take_to_draw_buf failed");
        s_state = State::Failed;
        return;
    }
    s_state = State::Captured;
}

#endif  // WAVEX_ESP_SCREENSHOT_DEBUG
