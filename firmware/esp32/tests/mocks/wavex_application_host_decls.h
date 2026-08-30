#pragma once

// Forced-include shim for compiling wavex_application.cpp on the host
// (see CMakeLists.txt). pcnt_task.h and ui_task.h only declare these when
// building for the target, but wavex_application.cpp still calls them in the
// WAVEX_TEST_BUILD configuration. Mock definitions live in esp32_mocks.cpp.

#include "esp_err.h"

namespace WaveX {
namespace Comm {
class ICommInterface;
}
}  // namespace WaveX

esp_err_t pcnt_task_init(void);
esp_err_t pcnt_task_start(void);
esp_err_t wavex_ui_task_start(WaveX::Comm::ICommInterface& comm_interface);

// ESP-IDF helpers wavex_application.cpp uses that the mock headers it
// includes do not declare (definitions in esp32_mocks.cpp).
unsigned int esp_get_free_heap_size();
const char* esp_err_to_name(esp_err_t code);

#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(xTimeInMs) ((xTimeInMs) / 10)
#endif
