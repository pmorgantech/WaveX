#include "panel_task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_backend.h"
#include "panel/pot_service.h"
#include "panel_spi.h"
#include "pcnt_task.h"

#include <atomic>
namespace wavex_panel {
namespace {
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
wavex_ui::PanelLedFrame published;
uint32_t published_at = 0;
Status status;
std::atomic<TaskHandle_t> task{nullptr};
std::atomic<bool> running{false};
uint32_t nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}
void run(void*) {
    // Start publishes the handle before the worker may finish.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const auto& backend = SelectedLedBackend();
    InitPots();
    Status local;
    bool attempted = false;
    uint32_t checked_at = nowMs() - 20, attempted_at = 0;
    while (running.load()) {
        pcnt_poll();
        ServicePots(nowMs());
        const uint32_t now = nowMs();
        if (static_cast<uint32_t>(now - checked_at) >= 20) {
            checked_at = now;
            wavex_ui::PanelLedFrame next;
            uint32_t heartbeat;
            portENTER_CRITICAL(&mux);
            next = published;
            heartbeat = published_at;
            portEXIT_CRITICAL(&mux);
            next = wavex_ui::FreshPanelLedFrame(next, nowMs(), heartbeat);
            // Init errors are retried at most once per second. A disabled or
            // deliberately stubbed backend leaves the encoder service alive.
            if (!local.ready && (!attempted || now - attempted_at >= 1000)) {
                attempted = true;
                attempted_at = now;
                const esp_err_t result = backend.init();
                local.ready = result == ESP_OK;
                local.applied = false;
                if (!local.ready) {
                    ++local.errors;
                    if (local.errors == 1)
                        ESP_LOGW(
                            "PANEL", "%s unavailable: %s", backend.name, esp_err_to_name(result));
                }
            }
            if (local.ready && (!local.applied || next != local.frame)) {
                if (backend.write(next) == ESP_OK) {
                    local.frame = next;
                    local.applied = true;
                    ++local.writes;
                } else {
                    ++local.errors;
                    local.applied = local.ready = false;
                    backend.shutdown();  // blank before retry; never advertise stale success
                    attempted_at = now;
                }
            }
            portENTER_CRITICAL(&mux);
            status = local;
            portEXIT_CRITICAL(&mux);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    backend.shutdown();
    StopPots();
    CloseSpiBus();
    portENTER_CRITICAL(&mux);
    status.ready = status.applied = false;
    portEXIT_CRITICAL(&mux);
    task.store(nullptr);
    vTaskDelete(nullptr);
}
}  // namespace
esp_err_t Start() {
    if (task.load())
        return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&mux);
    published = {};
    published_at = 0;
    status = {};
    portEXIT_CRITICAL(&mux);
    running.store(true);
    TaskHandle_t handle = nullptr;
    if (xTaskCreate(run, "panel_task", 4096, nullptr, 5, &handle) != pdPASS) {
        running.store(false);
        return ESP_ERR_NO_MEM;
    }
    task.store(handle);
    xTaskNotifyGive(handle);
    return ESP_OK;
}
esp_err_t Stop() {
    running.store(false);
    for (int waited = 0; task.load() && waited < 500; waited += 10)
        vTaskDelay(pdMS_TO_TICKS(10));
    return task.load() ? ESP_ERR_TIMEOUT : ESP_OK;
}
void Publish(const wavex_ui::PanelLedFrame& frame, uint32_t now_ms) {
    portENTER_CRITICAL(&mux);
    published = frame;
    published_at = now_ms;
    portEXIT_CRITICAL(&mux);
}
Status ReadStatus() {
    portENTER_CRITICAL(&mux);
    const Status result = status;
    portEXIT_CRITICAL(&mux);
    return result;
}
const char* BackendName() {
    return SelectedLedBackend().name;
}
}  // namespace wavex_panel
