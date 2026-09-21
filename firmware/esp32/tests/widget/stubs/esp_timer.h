#pragma once
#include <lvgl.h>

#include <cstdint>
inline int64_t esp_timer_get_time() {
    return int64_t(lv_tick_get()) * 1000;
}
