#include "esp32_mocks.h"

#include "comm/i_comm_interface.h"
#include "comm/statistics.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/task.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <queue>
#include <thread>

// Mock FreeRTOS queue implementation
struct MockQueue {
    std::queue<std::vector<uint8_t>> items;
    size_t item_size;
    std::mutex mutex;
};

static std::map<QueueHandle_t, MockQueue*> g_queues;
static std::mutex g_queues_mutex;
static uint32_t g_queue_counter = 1;

QueueHandle_t xQueueCreate(uint32_t uxQueueLength, uint32_t uxItemSize) {
    MockQueue* queue = new MockQueue;
    queue->item_size = uxItemSize;
    // Pre-allocate some items for testing
    for (uint32_t i = 0; i < uxQueueLength; ++i) {
        queue->items.push(std::vector<uint8_t>(uxItemSize));
    }

    QueueHandle_t handle = reinterpret_cast<QueueHandle_t>(g_queue_counter++);
    std::lock_guard<std::mutex> lock(g_queues_mutex);
    g_queues[handle] = queue;
    return handle;
}

BaseType_t xQueueSend(QueueHandle_t xQueue, const void* pvItemToQueue, TickType_t xTicksToWait) {
    std::lock_guard<std::mutex> lock(g_queues_mutex);
    auto it = g_queues.find(xQueue);
    if (it == g_queues.end())
        return pdFALSE;

    MockQueue* queue = it->second;
    std::lock_guard<std::mutex> queue_lock(queue->mutex);

    std::vector<uint8_t> item(queue->item_size);
    memcpy(item.data(), pvItemToQueue, queue->item_size);
    queue->items.push(item);
    return pdTRUE;
}

BaseType_t xQueueSendFromISR(QueueHandle_t xQueue,
                             const void* pvItemToQueue,
                             BaseType_t* pxHigherPriorityTaskWoken) {
    return xQueueSend(xQueue, pvItemToQueue, 0);
}

BaseType_t xQueueReceive(QueueHandle_t xQueue, void* pvBuffer, TickType_t xTicksToWait) {
    std::lock_guard<std::mutex> lock(g_queues_mutex);
    auto it = g_queues.find(xQueue);
    if (it == g_queues.end())
        return pdFALSE;

    MockQueue* queue = it->second;
    std::lock_guard<std::mutex> queue_lock(queue->mutex);

    if (queue->items.empty()) {
        if (xTicksToWait > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(xTicksToWait));
        }
        return pdFALSE;
    }

    std::vector<uint8_t> item = queue->items.front();
    queue->items.pop();
    memcpy(pvBuffer, item.data(), queue->item_size);
    return pdTRUE;
}

BaseType_t xQueueReceiveFromISR(QueueHandle_t xQueue,
                                void* pvBuffer,
                                BaseType_t* pxHigherPriorityTaskWoken) {
    return xQueueReceive(xQueue, pvBuffer, 0);
}

uint32_t uxQueueMessagesWaiting(QueueHandle_t xQueue) {
    std::lock_guard<std::mutex> lock(g_queues_mutex);
    auto it = g_queues.find(xQueue);
    if (it == g_queues.end())
        return 0;

    std::lock_guard<std::mutex> queue_lock(it->second->mutex);
    return static_cast<uint32_t>(it->second->items.size());
}

void vQueueDelete(QueueHandle_t xQueue) {
    std::lock_guard<std::mutex> lock(g_queues_mutex);
    auto it = g_queues.find(xQueue);
    if (it != g_queues.end()) {
        delete it->second;
        g_queues.erase(it);
    }
}

// Mock task functions
TaskHandle_t xTaskCreate(void (*pxTaskCode)(void*),
                         const char* pcName,
                         uint32_t usStackDepth,
                         void* pvParameters,
                         uint32_t uxPriority,
                         TaskHandle_t* pxCreatedTask) {
    // In tests, tasks run synchronously
    if (pxTaskCode) {
        pxTaskCode(pvParameters);
    }
    return reinterpret_cast<TaskHandle_t>(1);
}

void vTaskDelete(TaskHandle_t xTask) {
    // No-op in tests
}

void vTaskDelay(TickType_t xTicksToDelay) {
    std::this_thread::sleep_for(std::chrono::milliseconds(xTicksToDelay));
}

static uint32_t g_tick_count = 0;
TickType_t xTaskGetTickCount(void) {
    return g_tick_count++;
}

// Mock semaphore functions
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return reinterpret_cast<SemaphoreHandle_t>(1);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xBlockTime) {
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore) {
    return pdTRUE;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore,
                                 BaseType_t* pxHigherPriorityTaskWoken) {
    return pdTRUE;
}

// The host suite is single-threaded, so these only need to satisfy the caller:
// take always succeeds and there is no state to keep. What the tests exercise
// is the surrounding ordering (pair swapped as a unit, callback observed under
// the lock), not FreeRTOS itself.
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
    return reinterpret_cast<SemaphoreHandle_t>(1);
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t xSemaphore, TickType_t xBlockTime) {
    return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t xSemaphore) {
    return pdTRUE;
}

// Mock GPIO functions
static std::map<gpio_num_t, int> g_gpio_levels;
static std::map<gpio_num_t, gpio_mode_t> g_gpio_modes;

int gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode) {
    g_gpio_modes[gpio_num] = mode;
    return 0;
}

int gpio_set_level(gpio_num_t gpio_num, int level) {
    g_gpio_levels[gpio_num] = level;
    return 0;
}

int gpio_get_level(gpio_num_t gpio_num) {
    return g_gpio_levels[gpio_num];
}

// Mock UART functions
static std::map<uart_port_t, std::queue<uint8_t>> g_uart_rx_buffers;
static std::map<uart_port_t, std::vector<uint8_t>> g_uart_tx_buffers;

int uart_driver_install(uart_port_t uart_num,
                        int rx_buffer_size,
                        int tx_buffer_size,
                        int queue_size,
                        void* queue,
                        int intr_alloc_flags) {
    return 0;
}

int uart_param_config(uart_port_t uart_num, const uart_config_t* uart_config) {
    return 0;
}

int uart_set_pin(
    uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num) {
    return 0;
}

int uart_write_bytes(uart_port_t uart_num, const void* src, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(src);
    g_uart_tx_buffers[uart_num].insert(g_uart_tx_buffers[uart_num].end(), bytes, bytes + size);
    return static_cast<int>(size);
}

int uart_read_bytes(uart_port_t uart_num, void* buf, uint32_t length, TickType_t ticks_to_wait) {
    auto& rx_buffer = g_uart_rx_buffers[uart_num];
    size_t read = 0;
    uint8_t* bytes = static_cast<uint8_t*>(buf);

    while (!rx_buffer.empty() && read < length) {
        bytes[read++] = rx_buffer.front();
        rx_buffer.pop();
    }

    return static_cast<int>(read);
}

// Mock DMA functions
void* heap_caps_malloc(size_t size, uint32_t caps) {
    return malloc(size);
}

void heap_caps_free(void* ptr) {
    free(ptr);
}

// Mock timer functions
int esp_timer_create(const esp_timer_create_args_t* create_args, esp_timer_handle_t* out_handle) {
    *out_handle = reinterpret_cast<esp_timer_handle_t>(1);
    return 0;
}

int esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us) {
    return 0;
}

int esp_timer_stop(esp_timer_handle_t timer) {
    return 0;
}

int esp_timer_delete(esp_timer_handle_t timer) {
    return 0;
}

int64_t esp_timer_get_time(void) {
    static int64_t time = 0;
    return time++;
}

// Mock ESP logging functions
static esp_log_level_t g_log_level = ESP_LOG_INFO;

void esp_log_level_set(const char* tag, esp_log_level_t level) {
    (void)tag;  // Suppress unused parameter
    g_log_level = level;
}

esp_log_level_t esp_log_level_get(const char* tag) {
    (void)tag;  // Suppress unused parameter
    return g_log_level;
}

// Mock FreeRTOS task.h functions
eTaskState eTaskGetState(TaskHandle_t xTask) {
    (void)xTask;
    return eRunning;
}

const char* pcTaskGetName(TaskHandle_t xTask) {
    (void)xTask;
    return "mock_task";
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask) {
    (void)xTask;
    return 1024;  // Mock stack high water mark
}

// Mock inter_mcu functions. These are called from packet_router.cpp's real
// handlers; every call is recorded so tests can assert content at the
// production/inter_mcu boundary rather than just "did not crash".
namespace WaveX {
namespace Test {

InterMcuCapture& GetInterMcuCapture() {
    static InterMcuCapture capture;
    return capture;
}

void ResetInterMcuCapture() {
    GetInterMcuCapture() = InterMcuCapture{};
}

}  // namespace Test
}  // namespace WaveX

using WaveX::Test::GetInterMcuCapture;

void inter_mcu_update_backend_meters(float rms_left,
                                     float rms_right,
                                     float peak_left,
                                     float peak_right) {
    auto& cap = GetInterMcuCapture();
    cap.meter_calls++;
    cap.meter_rms_left = rms_left;
    cap.meter_rms_right = rms_right;
    cap.meter_peak_left = peak_left;
    cap.meter_peak_right = peak_right;
}

void inter_mcu_invoke_browse_resp_callback(const uint8_t* data, size_t length) {
    auto& cap = GetInterMcuCapture();
    cap.browse_resp_calls++;
    cap.browse_resp_data.assign(data, data + length);
}

void inter_mcu_invoke_storage_status_callback(bool mounted) {
    auto& cap = GetInterMcuCapture();
    cap.storage_status_calls++;
    cap.storage_status_mounted = mounted;
}

void inter_mcu_store_diag_push(const WaveX::Protocol::DiagPushMessage& msg) {
    auto& cap = GetInterMcuCapture();
    cap.diag_push_calls++;
    cap.last_diag_push = msg;
}

bool inter_mcu_get_diag_push(WaveX::Protocol::DiagPushMessage* out, uint32_t max_age_ms) {
    (void)out;
    (void)max_age_ms;
    return false;
}

void inter_mcu_store_sample_meta(const WaveX::Protocol::SampleMetadata& msg) {
    auto& cap = GetInterMcuCapture();
    cap.sample_meta_calls++;
    cap.last_sample_meta = msg;
}

void inter_mcu_store_track_binding(const WaveX::Protocol::TrackBindingMessage& msg) {
    auto& cap = GetInterMcuCapture();
    cap.track_binding_calls++;
    cap.last_track_binding = msg;
}

bool inter_mcu_get_track_binding(uint8_t track, WaveX::Protocol::TrackBindingMessage* out) {
    (void)track;
    (void)out;
    return false;
}

esp_err_t inter_mcu_request_track_binding(uint8_t track) {
    (void)track;
    return ESP_OK;
}

esp_err_t inter_mcu_request_sample_meta_page(uint16_t first, uint8_t count) {
    (void)first;
    (void)count;
    return ESP_OK;
}

void inter_mcu_store_sample_meta_page(const WaveX::Protocol::SampleMetaPageHeader& header,
                                      const uint8_t* records) {
    (void)header;
    (void)records;
}

size_t inter_mcu_get_sample_meta_page(WaveX::Protocol::SampleMetadata* out,
                                      size_t max,
                                      uint16_t* total,
                                      uint16_t* first) {
    (void)out;
    (void)max;
    if (total)
        *total = 0;
    if (first)
        *first = 0;
    return 0;
}

size_t inter_mcu_sample_meta_snapshot(WaveX::Protocol::SampleMetadata* out, size_t max) {
    (void)out;
    (void)max;
    return 0;
}

bool inter_mcu_get_sample_meta(uint16_t sample_id, WaveX::Protocol::SampleMetadata* out) {
    (void)sample_id;
    (void)out;
    return false;
}

esp_err_t inter_mcu_request_sample_meta(uint16_t sample_id) {
    (void)sample_id;
    return ESP_OK;
}

esp_err_t inter_mcu_send_sample_edit(uint8_t slot,
                                     bool loop_enabled,
                                     int16_t gain_db_x10,
                                     uint32_t start_frame,
                                     uint32_t end_frame,
                                     uint32_t loop_start,
                                     uint32_t loop_end,
                                     uint16_t fade_in_ms,
                                     uint16_t fade_out_ms) {
    (void)slot;
    (void)loop_enabled;
    (void)gain_db_x10;
    (void)start_frame;
    (void)end_frame;
    (void)loop_start;
    (void)loop_end;
    (void)fade_in_ms;
    (void)fade_out_ms;
    return ESP_OK;
}

esp_err_t inter_mcu_send_diag_subscribe(bool enable, uint8_t interval_hz) {
    (void)enable;
    (void)interval_hz;
    return ESP_OK;
}

void inter_mcu_handle_sample_stop_response(bool success) {
    auto& cap = GetInterMcuCapture();
    cap.stop_resp_calls++;
    cap.stop_resp_success = success;
}

void inter_mcu_update_backend_heartbeat_detailed(uint32_t uptime_ms,
                                                 uint32_t rx_total,
                                                 uint32_t loop_counter,
                                                 float cpu_avg_percent,
                                                 float cpu_min_percent,
                                                 float cpu_max_percent) {
    auto& cap = GetInterMcuCapture();
    cap.heartbeat_calls++;
    cap.hb_uptime_ms = uptime_ms;
    cap.hb_rx_total = rx_total;
    cap.hb_loop_counter = loop_counter;
    cap.hb_cpu_avg = cpu_avg_percent;
    cap.hb_cpu_min = cpu_min_percent;
    cap.hb_cpu_max = cpu_max_percent;
}

// Additional ESP-IDF mock functions
unsigned int esp_get_free_heap_size() {
    return 1024 * 1024;  // Mock 1MB free heap
}

extern "C" const esp_app_desc_t* esp_app_get_description(void) {
    static const esp_app_desc_t desc = {
        "0.0.0-test", "wavex-test", "00:00:00", "1970-01-01", "mock"};
    return &desc;
}

const char* esp_err_to_name(esp_err_t code) {
    switch (code) {
        case ESP_OK:
            return "ESP_OK";
        case ESP_FAIL:
            return "ESP_FAIL";
        default:
            return "UNKNOWN_ERROR";
    }
}

// WaveXApplication lifecycle mocks. Results are configurable through
// InterMcuCapture so init-failure ordering can be tested.
esp_err_t inter_mcu_init(StatisticsManager& statistics) {
    (void)statistics;
    auto& cap = GetInterMcuCapture();
    cap.inter_mcu_init_calls++;
    return cap.inter_mcu_init_result;
}

esp_err_t inter_mcu_start() {
    auto& cap = GetInterMcuCapture();
    cap.inter_mcu_start_calls++;
    return cap.inter_mcu_start_result;
}

esp_err_t pcnt_task_init() {
    auto& cap = GetInterMcuCapture();
    cap.pcnt_init_calls++;
    return cap.pcnt_init_result;
}

esp_err_t pcnt_task_start() {
    auto& cap = GetInterMcuCapture();
    cap.pcnt_start_calls++;
    return cap.pcnt_start_result;
}

esp_err_t wavex_ui_task_start(WaveX::Comm::ICommInterface& comm_interface) {
    (void)comm_interface;
    auto& cap = GetInterMcuCapture();
    cap.ui_start_calls++;
    return cap.ui_start_result;
}

// Additional inter_mcu mock functions for CommInterfaceImpl
esp_err_t inter_mcu_send_sample_play_index_req(uint32_t file_index, uint16_t loop_gap_ms) {
    (void)loop_gap_ms;
    auto& cap = GetInterMcuCapture();
    cap.play_index_req_calls++;
    cap.play_index_file_index = file_index;
    return cap.send_result;
}

esp_err_t inter_mcu_send_sample_stop_req() {
    auto& cap = GetInterMcuCapture();
    cap.stop_req_calls++;
    return cap.send_result;
}

bool inter_mcu_is_busy() {
    return GetInterMcuCapture().busy;
}

// FreeRTOS tick conversion macros are defined in esp32_mocks.h

esp_err_t inter_mcu_send_envelope_req(uint16_t sample_id,
                                      uint16_t columns,
                                      uint32_t start_frame,
                                      uint32_t end_frame) {
    (void)sample_id;
    (void)columns;
    (void)start_frame;
    (void)end_frame;
    return ESP_OK;
}
