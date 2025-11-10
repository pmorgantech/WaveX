#ifndef ESP_TIMER_H
#define ESP_TIMER_H

#include "esp_err.h"

#include <cstdint>

// Mock ESP timer types (already defined in esp32_mocks.h, but need header)
typedef void* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void* arg);

typedef struct {
    esp_timer_cb_t callback;
    void* arg;
    const char* name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

// Mock timer functions (implementations in esp32_mocks.cpp)
esp_err_t esp_timer_create(const esp_timer_create_args_t* create_args,
                           esp_timer_handle_t* out_handle);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
int64_t esp_timer_get_time(void);

#endif  // ESP_TIMER_H
