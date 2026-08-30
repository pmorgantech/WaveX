#ifndef ESP32_TEST_MOCKS_H
#define ESP32_TEST_MOCKS_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>

// Include FreeRTOS mock types (don't redeclare functions here)
#include "freertos/FreeRTOS.h"

// Mock ESP-IDF logging
#include "esp_log.h"

// Mock ESP-IDF GPIO
typedef enum { GPIO_NUM_0 = 0, GPIO_NUM_MAX } gpio_num_t;

typedef enum { GPIO_MODE_INPUT = 0, GPIO_MODE_OUTPUT } gpio_mode_t;

extern int gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode);
extern int gpio_set_level(gpio_num_t gpio_num, int level);
extern int gpio_get_level(gpio_num_t gpio_num);

// Mock ESP-IDF UART
typedef int uart_port_t;
#define UART_NUM_0 0
#define UART_NUM_1 1
#define UART_NUM_2 2

typedef struct {
    int baud_rate;
    int data_bits;
    int stop_bits;
    int parity;
    int flow_ctrl;
} uart_config_t;

extern int uart_driver_install(uart_port_t uart_num,
                               int rx_buffer_size,
                               int tx_buffer_size,
                               int queue_size,
                               void* queue,
                               int intr_alloc_flags);
extern int uart_param_config(uart_port_t uart_num, const uart_config_t* uart_config);
extern int uart_set_pin(
    uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num);
extern int uart_write_bytes(uart_port_t uart_num, const void* src, size_t size);
extern int uart_read_bytes(uart_port_t uart_num,
                           void* buf,
                           uint32_t length,
                           TickType_t ticks_to_wait);

// Mock ESP-IDF DMA
typedef void* dma_descriptor_t;
extern void* heap_caps_malloc(size_t size, uint32_t caps);
extern void heap_caps_free(void* ptr);

#define MALLOC_CAP_DMA 0x01
#define MALLOC_CAP_INTERNAL 0x02

// Mock ESP-IDF timer
#include "esp_timer.h"

// Mock UI task functions
void wavex_ui_mark_content_changed(void);

// FreeRTOS tick conversion macros
#define portTICK_PERIOD_MS 10
#define pdMS_TO_TICKS(xTimeInMs) ((TickType_t)((xTimeInMs) / portTICK_PERIOD_MS))

// FreeRTOS return values
#define pdTRUE 1
#define pdFALSE 0

// ---------------------------------------------------------------------------
// inter_mcu boundary capture
// ---------------------------------------------------------------------------
// The inter_mcu_* mock implementations record every call into this struct so
// tests can assert what actually crossed the production/inter_mcu boundary
// (packet_router's real handlers convert and validate before calling these; a
// discarded call proves nothing). Outbound send mocks also honour
// `send_result`, letting tests exercise the link-down/error propagation paths
// of CommInterfaceImpl.

#include "../../../shared/spi_protocol/protocol.h"
#include "esp_err.h"

#include <vector>

namespace WaveX {
namespace Test {

struct InterMcuCapture {
    // --- inbound: packet_router handlers -> inter_mcu_* ---
    int heartbeat_calls = 0;
    uint32_t hb_uptime_ms = 0;
    uint32_t hb_rx_total = 0;
    uint32_t hb_loop_counter = 0;
    float hb_cpu_avg = 0.0f;
    float hb_cpu_min = 0.0f;
    float hb_cpu_max = 0.0f;

    int meter_calls = 0;
    float meter_rms_left = 0.0f;
    float meter_rms_right = 0.0f;
    float meter_peak_left = 0.0f;
    float meter_peak_right = 0.0f;

    int browse_resp_calls = 0;
    std::vector<uint8_t> browse_resp_data;

    int wave_chunk_calls = 0;
    uint32_t wave_chunk_offset = 0;
    std::vector<int16_t> wave_chunk_samples;

    int envelope_chunk_calls = 0;
    WaveX::Protocol::EnvelopeChunkMessage envelope_header;
    std::vector<WaveX::Protocol::EnvelopeColumn> envelope_columns;

    int sample_status_calls = 0;
    uint16_t sample_status_id = 0;
    uint8_t sample_status_state = 0;
    uint32_t sample_status_rate = 0;
    uint8_t sample_status_channels = 0;
    uint32_t sample_status_frames = 0;

    int storage_status_calls = 0;
    bool storage_status_mounted = false;

    int stop_resp_calls = 0;
    bool stop_resp_success = false;

    int diag_push_calls = 0;
    WaveX::Protocol::DiagPushMessage last_diag_push;

    int sample_meta_calls = 0;
    WaveX::Protocol::SampleMetadata last_sample_meta;

    int sample_mem_status_calls = 0;
    int cv_cal_calls = 0;

    // --- outbound: CommInterfaceImpl -> inter_mcu_send_* ---
    esp_err_t send_result = ESP_OK;  // returned by every inter_mcu_send_* mock
    int browse_req_calls = 0;
    char browse_req_path[96] = {0};
    uint8_t browse_req_start_index = 0;

    int play_index_req_calls = 0;
    uint32_t play_index_file_index = 0;

    int stop_req_calls = 0;

    int sample_data_calls = 0;
    std::vector<uint8_t> sample_data;

    bool busy = false;  // returned by inter_mcu_is_busy()

    // --- WaveXApplication lifecycle mocks ---
    esp_err_t inter_mcu_init_result = ESP_OK;
    esp_err_t inter_mcu_start_result = ESP_OK;
    esp_err_t pcnt_init_result = ESP_OK;
    esp_err_t pcnt_start_result = ESP_OK;
    esp_err_t ui_start_result = ESP_OK;
    int inter_mcu_init_calls = 0;
    int inter_mcu_start_calls = 0;
    int pcnt_init_calls = 0;
    int pcnt_start_calls = 0;
    int ui_start_calls = 0;
};

InterMcuCapture& GetInterMcuCapture();
void ResetInterMcuCapture();

}  // namespace Test
}  // namespace WaveX

#endif  // ESP32_TEST_MOCKS_H
