#include "esp32_mocks.h"

#include "comm/i_comm_interface.h"
#include "comm/statistics.h"
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
    return it->second->items.size();
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
    return size;
}

int uart_read_bytes(uart_port_t uart_num, void* buf, uint32_t length, TickType_t ticks_to_wait) {
    auto& rx_buffer = g_uart_rx_buffers[uart_num];
    size_t read = 0;
    uint8_t* bytes = static_cast<uint8_t*>(buf);

    while (!rx_buffer.empty() && read < length) {
        bytes[read++] = rx_buffer.front();
        rx_buffer.pop();
    }

    return read;
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

// Mock inter_mcu functions (stubs for testing)
// These are called from packet_router.cpp
void inter_mcu_update_backend_meters(float rms_left,
                                     float rms_right,
                                     float peak_left,
                                     float peak_right) {
    (void)rms_left;
    (void)rms_right;
    (void)peak_left;
    (void)peak_right;
    // Stub implementation for tests
}

void inter_mcu_invoke_browse_resp_callback(const uint8_t* data, size_t length) {
    (void)data;
    (void)length;
    // Stub implementation for tests
}

void inter_mcu_handle_sample_stop_response(bool success) {
    (void)success;
    // Stub implementation for tests
}

void inter_mcu_update_backend_heartbeat_detailed(uint32_t uptime_ms,
                                                 uint32_t rx_total,
                                                 uint32_t loop_counter,
                                                 float cpu_avg_percent,
                                                 float cpu_min_percent,
                                                 float cpu_max_percent) {
    (void)uptime_ms;
    (void)rx_total;
    (void)loop_counter;
    (void)cpu_avg_percent;
    (void)cpu_min_percent;
    (void)cpu_max_percent;
    // Stub implementation for tests
}

// Additional ESP-IDF mock functions
unsigned int esp_get_free_heap_size() {
    return 1024 * 1024;  // Mock 1MB free heap
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

// Additional mock functions for WaveXApplication tests
esp_err_t inter_mcu_init(StatisticsManager& statistics) {
    (void)statistics;
    return ESP_OK;
}

esp_err_t inter_mcu_start() {
    return ESP_OK;
}

esp_err_t pcnt_task_init() {
    return ESP_OK;
}

esp_err_t pcnt_task_start() {
    return ESP_OK;
}

esp_err_t wavex_ui_task_start(WaveX::Comm::ICommInterface& comm_interface) {
    (void)comm_interface;
    return ESP_OK;
}

// Additional inter_mcu mock functions for CommInterfaceImpl
esp_err_t inter_mcu_send_sample_play_index_req(uint32_t file_index) {
    (void)file_index;
    return ESP_OK;
}

esp_err_t inter_mcu_send_sample_stop_req() {
    return ESP_OK;
}

bool inter_mcu_is_busy() {
    return false;
}

// FreeRTOS tick conversion macros are defined in esp32_mocks.h
