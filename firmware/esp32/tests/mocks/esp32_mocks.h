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

#endif  // ESP32_TEST_MOCKS_H
