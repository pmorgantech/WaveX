#pragma once
// Narrow adapter for pot-service ownership tests. Real std::mutex preserves the
// critical-section contract; no fake SPI/NVS work occurs under this lock.
#include <mutex>
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED \
    {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
