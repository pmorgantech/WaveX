#pragma once
#include <mutex>
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED \
    {}
#define taskENTER_CRITICAL(mux) (mux)->lock()
#define taskEXIT_CRITICAL(mux) (mux)->unlock()
