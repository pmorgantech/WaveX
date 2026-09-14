#pragma once
#include <cstdint>
// Host tests execute LVGL and navigation synchronously on one thread.
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
inline bool lvgl_port_lock(uint32_t) {
    return true;
}
inline void lvgl_port_unlock() {}
