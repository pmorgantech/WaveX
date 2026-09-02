#include "inter_mcu.h"

#include <string.h>

#include "../../shared/config/link_config.h"
#include "../../shared/spi_protocol/protocol.h"
#include "../../shared/uart_protocol/uart_protocol.h"
#include "comm/listener_slot.h"
#include "comm/statistics.h"
#include "links/esp_uart_link.h"

#include <atomic>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#else
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("[%s] DEBUG: " fmt "\n", tag, ##__VA_ARGS__)
#endif

static const char* TAG = "InterMCU";

// UART link state
static bool s_uart_initialized = false;
static bool s_uart_started = false;

// Statistics tracking (injected dependency)
static StatisticsManager* s_statistics = nullptr;

// Communication state. Read from every public entry point below (called from
// whichever task owns the caller - UI task, comm callbacks) and written from
// init()/deinit()/suspend(); atomic per docs/esp32p4_coding_guide.md SS9.
static std::atomic<bool> s_suspended{false};
static std::atomic<bool> s_initialized{false};

// Cached sample memory diagnostics
static wavex_sample_mem_status_t s_sample_mem_status = {};
static portMUX_TYPE s_sample_mem_lock = portMUX_INITIALIZER_UNLOCKED;
// Pages register these with `this` and clear them in onExit; the UART task
// invokes them. ListenerSlot makes the pair swap atomic and makes a clear
// block until any in-flight callback has returned - see listener_slot.h.
static WaveX::Comm::ListenerSlot<wavex_wave_chunk_cb_t> s_wave_chunk_listener;
static WaveX::Comm::ListenerSlot<wavex_envelope_chunk_cb_t> s_envelope_chunk_listener;
static WaveX::Comm::ListenerSlot<wavex_inst_status_cb_t> s_inst_status_listener;

static int send_uart_message(uint8_t msg_type, const void* payload, uint16_t len) {
    if (!s_uart_initialized || !s_uart_started) {
        ESP_LOGE(TAG, "UART link not ready (msg=0x%02X)", msg_type);
        return -1;
    }

    int result = uart_link_send(msg_type, payload, len);
    if (result < 0) {
        ESP_LOGE(TAG, "UART send failed (msg=0x%02X)", msg_type);
    }
    return result;
}

esp_err_t inter_mcu_init(StatisticsManager& statistics) {
    if (s_initialized) {
        ESP_LOGI(TAG, "Inter-MCU communication already initialized");
        return ESP_OK;
    }

    s_statistics = &statistics;

    ESP_LOGI(TAG, "Initializing inter-MCU communication (UART only)...");

    esp_err_t ret = uart_link_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART link initialization failed");
        return ret;
    }
    s_uart_initialized = true;
    ESP_LOGI(TAG, "Inter-MCU communication initialized successfully");
    s_initialized = true;

    return ESP_OK;
}

esp_err_t inter_mcu_start() {
    if (!s_initialized) {
        ESP_LOGE(TAG, "Inter-MCU communication not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting inter-MCU communication (UART only)...");

    esp_err_t ret = uart_link_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART link start failed");
        return ret;
    }
    s_uart_started = true;

    ESP_LOGI(TAG, "Inter-MCU communication started successfully");

    return ESP_OK;
}

static WaveX::Comm::ListenerSlot<wavex_cv_cal_cb_t> s_cv_cal_listener;

esp_err_t inter_mcu_send_cv_cal_set(const WaveX::Protocol::CvCalMessage& cal) {
    if (!s_initialized || s_suspended) {
        return ESP_FAIL;
    }
    int result = send_uart_message(WaveX::Protocol::MSG_CV_CAL_SET, &cal, sizeof(cal));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_cv_cal_get(uint8_t group) {
    if (!s_initialized || s_suspended) {
        return ESP_FAIL;
    }
    WaveX::Protocol::CvCalGetMessage msg(group);
    int result = send_uart_message(WaveX::Protocol::MSG_CV_CAL_GET, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_cv_test(const WaveX::Protocol::CvTestMessage& test) {
    if (!s_initialized || s_suspended) {
        return ESP_FAIL;
    }
    int result = send_uart_message(WaveX::Protocol::MSG_CV_TEST, &test, sizeof(test));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

void inter_mcu_set_cv_cal_listener(wavex_cv_cal_cb_t cb, void* user_data) {
    s_cv_cal_listener.set(cb, user_data);
}

void inter_mcu_invoke_cv_cal_callback(const WaveX::Protocol::CvCalMessage& cal) {
    s_cv_cal_listener.invoke(cal);
}

esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::ControlChangeMessage msg;
    msg.parameter = parameter;
    msg.channel = channel;
    msg.value = value;

    int result = send_uart_message(WaveX::Protocol::MSG_CONTROL_CHANGE, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::NoteMessage msg;
    msg.note = note;
    msg.velocity = velocity;
    msg.channel = channel;
    msg.reserved = 0;

    int result = send_uart_message(WaveX::Protocol::MSG_NOTE_ON, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::NoteMessage msg;
    msg.note = note;
    msg.velocity = 0;  // Note off
    msg.channel = channel;
    msg.reserved = 0;

    int result = send_uart_message(WaveX::Protocol::MSG_NOTE_OFF, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_sample_ctrl(uint8_t slot, wavex_sample_ctrl_cmd_t cmd, float rate) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::SampleCtrlMessage msg;
    msg.slot = slot;
    msg.cmd = static_cast<uint8_t>(cmd);
    msg.rate = rate;

    int result = send_uart_message(WaveX::Protocol::MSG_SAMPLE_CTRL, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::PreviewReqMessage msg;
    msg.slot = slot;
    msg.start = start;
    msg.end = end;
    msg.decim = decim;

    int result = send_uart_message(WaveX::Protocol::MSG_PREVIEW_REQ, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_envelope_req(uint16_t sample_id,
                                      uint16_t columns,
                                      uint32_t start_frame,
                                      uint32_t end_frame) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::EnvelopeReqMessage msg(sample_id, columns, start_frame, end_frame);
    int result = send_uart_message(WaveX::Protocol::MSG_ENVELOPE_REQ, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

namespace {
// Small fixed cache. The Daisy holds a bounded number of loaded samples, so a
// fixed ring here cannot fall behind it, and a fixed array avoids allocating
// from the UART RX task.
constexpr size_t kMetaCacheSize = 8;
portMUX_TYPE s_meta_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::SampleMetadata s_meta[kMetaCacheSize];
bool s_meta_valid[kMetaCacheSize] = {};
size_t s_meta_next = 0;
uint16_t s_meta_newest_id = 0;
}  // namespace

void inter_mcu_store_sample_meta(const WaveX::Protocol::SampleMetadata& msg) {
    taskENTER_CRITICAL(&s_meta_lock);
    size_t slot = kMetaCacheSize;
    for (size_t i = 0; i < kMetaCacheSize; ++i) {
        if (s_meta_valid[i] && s_meta[i].sample_id == msg.sample_id) {
            slot = i;  // update in place, so an edit does not consume a slot
            break;
        }
    }
    if (slot == kMetaCacheSize) {
        slot = s_meta_next;
        s_meta_next = (s_meta_next + 1) % kMetaCacheSize;
    }
    s_meta[slot] = msg;
    s_meta_valid[slot] = true;
    s_meta_newest_id = msg.sample_id;
    taskEXIT_CRITICAL(&s_meta_lock);
}

bool inter_mcu_get_sample_meta(uint16_t sample_id, WaveX::Protocol::SampleMetadata* out) {
    if (!out) {
        return false;
    }
    bool found = false;
    taskENTER_CRITICAL(&s_meta_lock);
    const uint16_t want = sample_id ? sample_id : s_meta_newest_id;
    for (size_t i = 0; i < kMetaCacheSize; ++i) {
        if (s_meta_valid[i] && s_meta[i].sample_id == want) {
            *out = s_meta[i];
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_meta_lock);
    return found;
}

esp_err_t inter_mcu_request_sample_meta(uint16_t sample_id) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    WaveX::Protocol::SampleMetaReqMessage msg(sample_id);
    int result = send_uart_message(WaveX::Protocol::MSG_SAMPLE_META_REQ, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_sample_select(uint16_t sample_id) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    WaveX::Protocol::SampleSelectMessage msg(sample_id);
    int result = send_uart_message(WaveX::Protocol::MSG_SAMPLE_SELECT, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_sample_unload(uint16_t sample_id) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_id == 0) {
        return ESP_ERR_INVALID_ARG;  // the backend rejects it; fail here rather than round-trip
    }
    WaveX::Protocol::SampleUnloadMessage msg(sample_id);
    int result = send_uart_message(WaveX::Protocol::MSG_SAMPLE_UNLOAD, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
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
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    WaveX::Protocol::SampleEditMessage msg(slot,
                                           loop_enabled ? 1 : 0,
                                           gain_db_x10,
                                           start_frame,
                                           end_frame,
                                           loop_start,
                                           loop_end,
                                           fade_in_ms,
                                           fade_out_ms);
    int result = send_uart_message(WaveX::Protocol::MSG_SAMPLE_EDIT_SET, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_diag_subscribe(bool enable, uint8_t interval_hz) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    WaveX::Protocol::DiagSubscribeMessage msg(enable ? 1 : 0, interval_hz);
    int result = send_uart_message(WaveX::Protocol::MSG_DIAG_SUBSCRIBE, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

namespace {
// Written by the UART RX task, read by the UI task. A portMUX critical
// section, matching the meter and heartbeat snapshots either side of it - the
// struct is 94 bytes, so a torn read would mix two intervals.
portMUX_TYPE s_diag_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::DiagPushMessage s_diag;
uint32_t s_diag_rx_ms = 0;
bool s_diag_valid = false;
}  // namespace

void inter_mcu_store_diag_push(const WaveX::Protocol::DiagPushMessage& msg) {
    taskENTER_CRITICAL(&s_diag_lock);
    s_diag = msg;
    s_diag_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_diag_valid = true;
    taskEXIT_CRITICAL(&s_diag_lock);
}

bool inter_mcu_get_diag_push(WaveX::Protocol::DiagPushMessage* out, uint32_t max_age_ms) {
    if (!out) {
        return false;
    }
    taskENTER_CRITICAL(&s_diag_lock);
    const bool valid = s_diag_valid;
    const uint32_t rx_ms = s_diag_rx_ms;
    if (valid) {
        *out = s_diag;
    }
    taskEXIT_CRITICAL(&s_diag_lock);
    if (!valid) {
        return false;
    }
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return (now_ms - rx_ms) <= max_age_ms;
}

bool inter_mcu_is_busy() {
    if (!s_initialized) {
        return false;
    }

    // For now, UART link doesn't expose a busy state; return false
    return false;
}

void inter_mcu_set_suspended(bool suspended) {
    s_suspended = suspended;
    ESP_LOGI(TAG, "Inter-MCU communication %s", suspended ? "suspended" : "resumed");
}

esp_err_t inter_mcu_request_sample_mem_status() {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::StatusRequestMessage req{};
    req.category = WaveX::Protocol::STATUS_CATEGORY_SAMPLE_MEM;
    int result = send_uart_message(WaveX::Protocol::MSG_STATUS_REQUEST, &req, sizeof(req));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

namespace {

// Drops meta-cache entries for samples the backend no longer holds.
//
// The meta cache is otherwise add/update-only and there is no way to express a
// deletion through it: MSG_SAMPLE_META can only ever describe a sample that
// EXISTS, and a push of the remaining set says nothing about what left.
// Unloading the LAST sample pushes nothing at all, so even "replace the whole
// set on receive" would never fire. Without this, an unloaded (or evicted -
// that path notifies nobody at all) sample stayed cached forever, and every
// consumer reading the cache kept listing RAM that had been freed. That is what
// made Unload look like it did nothing: the backend really did free it, and the
// UI had no way to find out.
//
// SampleMemStatus is used as the authority because it is the only message that
// carries a COUNT plus every resident id, so it can express both deletion and
// "nothing is loaded".
//
// IMPORTANT: it can only do so when it is not truncated. The Daisy holds up to
// kLoadedSampleCapacity (32) samples but this message carries at most
// WAVEX_SAMPLE_STATUS_MAX_ENTRIES (8) and GetSampleMemStatus() clamps to that.
// A full list is therefore possibly-truncated, and pruning against it would
// drop live entries - the truncation keeps the FIRST 8 loaded, while this cache
// holds the 8 most recently pushed, so the two sets need not overlap. Prune
// only when the count proves the list complete.
// The comparison is done OUTSIDE the critical section and only the resulting
// flags are written inside it.
//
// The first version ran the whole nested scan - up to 8x8 comparisons plus a
// second pass - under taskENTER_CRITICAL, on the UART RX task. That disables
// interrupts on the core for the duration, and this runs on the path every
// sample-status message takes, including the burst a load emits. Long critical
// sections are exactly what docs/esp32p4_coding_guide.md and the esp32p4 skill
// warn against ("short portMUX_TYPE critical sections"), and a section held too
// long is how the interrupt watchdog fires.
//
// Reading s_meta[] unlocked is safe here: this task is the only writer, and a
// concurrent UI-task reader is also only reading. The worst a race can do is
// mark a slot that changed in between, which the next status corrects.
void prune_sample_meta_to(const wavex_sample_mem_status_t& status) {
    if (status.sample_count >= WAVEX_SAMPLE_STATUS_MAX_ENTRIES) {
        return;  // possibly truncated: cannot prove absence
    }

    bool drop[kMetaCacheSize] = {};
    for (size_t i = 0; i < kMetaCacheSize; ++i) {
        if (!s_meta_valid[i]) {
            continue;
        }
        bool live = false;
        for (uint8_t e = 0; e < status.sample_count; ++e) {
            if (status.entries[e].sample_id == s_meta[i].sample_id) {
                live = true;
                break;
            }
        }
        drop[i] = !live;
    }

    // "Newest" may have been what was just unloaded. Re-point it at the
    // backend's last entry (the most recently loaded, since loads append)
    // rather than leaving it naming freed memory - callers pass id 0 to mean
    // "whatever is current" and would otherwise get nothing, or worse, a stale
    // hit if the id were reused.
    bool newest_live = false;
    for (uint8_t e = 0; e < status.sample_count; ++e) {
        if (status.entries[e].sample_id == s_meta_newest_id) {
            newest_live = true;
            break;
        }
    }
    const uint16_t new_newest =
        status.sample_count > 0 ? status.entries[status.sample_count - 1].sample_id : 0;

    // Only the writes are guarded, so the section is a handful of stores.
    taskENTER_CRITICAL(&s_meta_lock);
    for (size_t i = 0; i < kMetaCacheSize; ++i) {
        if (drop[i]) {
            s_meta_valid[i] = false;
        }
    }
    if (!newest_live) {
        s_meta_newest_id = new_newest;
    }
    taskEXIT_CRITICAL(&s_meta_lock);
}

}  // namespace

void inter_mcu_update_sample_mem_status(const wavex_sample_mem_status_t& status) {
    taskENTER_CRITICAL(&s_sample_mem_lock);
    s_sample_mem_status = status;
    taskEXIT_CRITICAL(&s_sample_mem_lock);
    // Outside the lock above: this takes s_meta_lock, and nesting the two
    // spinlocks would create the only lock ordering in this file.
    prune_sample_meta_to(status);
}

void inter_mcu_get_sample_mem_status(wavex_sample_mem_status_t* out) {
    if (!out) {
        ESP_LOGE(TAG, "Invalid sample mem status output pointer");
        return;
    }

    taskENTER_CRITICAL(&s_sample_mem_lock);
    memcpy(out, &s_sample_mem_status, sizeof(s_sample_mem_status));
    taskEXIT_CRITICAL(&s_sample_mem_lock);
}

void inter_mcu_set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data) {
    s_wave_chunk_listener.set(cb, user_data);
    ESP_LOGI(TAG, "Wave chunk listener registered: %p", cb);
}

void inter_mcu_set_envelope_chunk_listener(wavex_envelope_chunk_cb_t cb, void* user_data) {
    s_envelope_chunk_listener.set(cb, user_data);
}

void inter_mcu_invoke_envelope_chunk_callback(const WaveX::Protocol::EnvelopeChunkMessage& header,
                                              const WaveX::Protocol::EnvelopeColumn* columns) {
    // No page open that wants a waveform is the normal case; not worth a log line.
    s_envelope_chunk_listener.invoke(header, columns);
}

void inter_mcu_invoke_browse_resp_callback(const uint8_t* data, size_t length) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    ESP_LOGD(TAG, "Invoking browse response callback with %d bytes", (int)length);
    s_statistics->invoke_browse_resp_callback(data, length);
}

void inter_mcu_invoke_storage_status_callback(bool mounted) {
    if (!s_statistics) {
        return;
    }
    s_statistics->invoke_storage_status_callback(mounted);
}

void inter_mcu_invoke_wave_chunk_callback(uint32_t offset, const int16_t* samples, uint16_t count) {
    if (!s_wave_chunk_listener.registered()) {
        ESP_LOGW(TAG, "Wave chunk received but no listener registered");
        return;
    }
    s_wave_chunk_listener.invoke(offset, samples, count);
}

void inter_mcu_set_sample_status_listener(wavex_sample_status_cb_t cb, void* user_data) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    s_statistics->set_sample_status_callback(cb, user_data);
    ESP_LOGI(TAG, "Sample status listener registered: %p", cb);
}

void inter_mcu_invoke_sample_status_callback(uint16_t sample_id,
                                             uint8_t state,
                                             uint32_t sample_rate,
                                             uint8_t channels,
                                             uint32_t frames_played) {
    if (s_statistics) {
        s_statistics->invoke_sample_status_callback(
            sample_id, state, sample_rate, channels, frames_played);
    }
}

void inter_mcu_set_inst_status_listener(wavex_inst_status_cb_t cb, void* user_data) {
    s_inst_status_listener.set(cb, user_data);
}

void inter_mcu_invoke_inst_status_callback(const WaveX::Protocol::InstStatusMessage& status) {
    s_inst_status_listener.invoke(status);
}

void inter_mcu_get_backend_heartbeat(wavex_backend_heartbeat_t* out) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    if (!out) {
        ESP_LOGE(TAG, "Invalid heartbeat output pointer");
        return;
    }

    uint32_t uptime_ms, rx_total, loop_counter, last_rx_ms;
    float cpu_usage_percent;
    bool valid;
    s_statistics->get_backend_heartbeat(
        &uptime_ms, &rx_total, &loop_counter, &last_rx_ms, &cpu_usage_percent, &valid);

    out->uptime_ms = uptime_ms;
    out->rx_total = rx_total;
    out->loop_counter = loop_counter;
    out->last_rx_ms = last_rx_ms;
    out->cpu_usage_percent = cpu_usage_percent;
    // For backward compatibility, set all CPU fields to the same value
    out->cpu_avg_percent = cpu_usage_percent;
    out->cpu_min_percent = cpu_usage_percent;
    out->cpu_max_percent = cpu_usage_percent;
    out->valid = valid;
}

void inter_mcu_get_backend_heartbeat_detailed(wavex_backend_heartbeat_t* out) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    if (!out) {
        ESP_LOGE(TAG, "Invalid heartbeat output pointer");
        return;
    }

    uint32_t uptime_ms, rx_total, loop_counter, last_rx_ms;
    float cpu_avg_percent, cpu_min_percent, cpu_max_percent;
    bool valid;
    s_statistics->get_backend_heartbeat_detailed(&uptime_ms,
                                                 &rx_total,
                                                 &loop_counter,
                                                 &last_rx_ms,
                                                 &cpu_avg_percent,
                                                 &cpu_min_percent,
                                                 &cpu_max_percent,
                                                 &valid);

    out->uptime_ms = uptime_ms;
    out->rx_total = rx_total;
    out->loop_counter = loop_counter;
    out->last_rx_ms = last_rx_ms;
    out->cpu_usage_percent = cpu_avg_percent;  // Legacy compatibility
    out->cpu_avg_percent = cpu_avg_percent;
    out->cpu_min_percent = cpu_min_percent;
    out->cpu_max_percent = cpu_max_percent;
    out->valid = valid;
}

void inter_mcu_get_packet_stats(wavex_packet_stats_t* out) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    if (!out) {
        ESP_LOGE(TAG, "Invalid packet stats output pointer");
        return;
    }

    s_statistics->get_packet_stats(out);
}

void inter_mcu_reset_packet_stats(void) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    s_statistics->reset_packet_stats();
    ESP_LOGI(TAG, "Packet statistics reset");
}

void inter_mcu_get_packet_summary(wavex_packet_summary_t* out) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    if (!out) {
        ESP_LOGE(TAG, "Invalid packet summary output pointer");
        return;
    }

    s_statistics->get_packet_summary(out);
}

uint32_t inter_mcu_get_meter_packet_count(void) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return 0;
    }
    return s_statistics->get_meter_packet_count();
}

uint32_t inter_mcu_get_total_packet_count(void) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return 0;
    }
    return s_statistics->get_total_packet_count();
}

int inter_mcu_format_packet_stats(char* buffer, size_t buffer_size) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return -1;
    }
    if (!buffer || buffer_size == 0) {
        ESP_LOGE(TAG, "Invalid buffer for packet stats formatting");
        return -1;
    }

    return s_statistics->format_packet_stats(buffer, buffer_size);
}

void inter_mcu_get_tx_stats(wavex_tx_stats_t* out) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    if (!out) {
        ESP_LOGE(TAG, "Invalid TX stats output pointer");
        return;
    }

    s_statistics->get_tx_stats(out);
}

void inter_mcu_update_backend_heartbeat(uint32_t uptime_ms,
                                        uint32_t rx_total,
                                        uint32_t loop_counter,
                                        float cpu_usage_percent) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    s_statistics->update_backend_heartbeat(uptime_ms, rx_total, loop_counter, cpu_usage_percent);
}

void inter_mcu_update_backend_heartbeat_detailed(uint32_t uptime_ms,
                                                 uint32_t rx_total,
                                                 uint32_t loop_counter,
                                                 float cpu_avg_percent,
                                                 float cpu_min_percent,
                                                 float cpu_max_percent) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    s_statistics->update_backend_heartbeat_detailed(
        uptime_ms, rx_total, loop_counter, cpu_avg_percent, cpu_min_percent, cpu_max_percent);
}

void inter_mcu_update_backend_meters(float rms_left,
                                     float rms_right,
                                     float peak_left,
                                     float peak_right) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    s_statistics->update_meter_data(rms_left, rms_right, peak_left, peak_right);
}

void inter_mcu_get_meter_data(wavex_meter_data_t* out) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    if (!out) {
        ESP_LOGE(TAG, "Invalid meter data output pointer");
        return;
    }

    s_statistics->get_meter_data(out);
}

void inter_mcu_increment_packet_stat(uint8_t packet_type) {
    if (!s_statistics) {
        ESP_LOGE(TAG, "StatisticsManager not initialized");
        return;
    }
    s_statistics->increment_packet_stat(packet_type);
}

esp_err_t inter_mcu_send_browse_req(const char* path, uint8_t start_index) {
    ESP_LOGD("inter_mcu",
             "inter_mcu_send_browse_req: path='%s', start_index=%d",
             path ? path : "NULL",
             start_index);

    if (!s_initialized) {
        ESP_LOGE("inter_mcu", "inter_mcu not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!path) {
        ESP_LOGE("inter_mcu", "path is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Flexible payload: [start_index][path bytes...][\0]
    const size_t path_len = strlen(path);
    const size_t payload_len = 1 + path_len + 1;  // start_index + path + null terminator

    // Fixed buffer, not std::vector: this is a send path, the maximum size is
    // known from the protocol, and <vector> was only included under
    // ESP_PLATFORM while the use was unconditional - so the non-ESP branch of
    // this translation unit could not compile at all. That went unnoticed
    // because the host tests exclude this file.
    if (payload_len > 1 + WaveX::Protocol::BROWSE_PATH_MAX + 1) {
        ESP_LOGE("inter_mcu", "browse path too long (%u)", (unsigned)path_len);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t payload[1 + WaveX::Protocol::BROWSE_PATH_MAX + 1] = {0};
    payload[0] = start_index;
    memcpy(&payload[1], path, path_len);
    payload[payload_len - 1] = '\0';

    for (size_t i = 0; i < payload_len && i < 24; i++) {
        ESP_LOGD("inter_mcu", "  [%d] = 0x%02X", (int)i, payload[i]);
    }

    int result = send_uart_message(WaveX::Protocol::MSG_BROWSE_REQ, payload, (uint16_t)payload_len);

    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_sample_play_index_req(uint32_t file_index, uint16_t loop_gap_ms) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::SamplePlayIndexMessage msg;
    msg.index = file_index;
    msg.loop_gap_ms = loop_gap_ms;

    int result = send_uart_message(WaveX::Protocol::MSG_SAMPLE_PLAY_INDEX_REQ, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_sample_stop_req() {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::SampleStopReqMessage msg;
    msg.slot = 0;  // Currently single slot; extend when multi-slot is supported
    msg.reserved[0] = 0;
    msg.reserved[1] = 0;
    msg.reserved[2] = 0;

    int result = send_uart_message(WaveX::Protocol::MSG_SAMPLE_STOP_REQ, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_sample_load_req(uint16_t sample_id,
                                         uint32_t sample_size,
                                         uint16_t sample_rate,
                                         uint8_t channels,
                                         uint8_t bit_depth,
                                         const char* path) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::SampleLoadMessage msg;
    msg.sample_id = sample_id;
    msg.sample_size = sample_size;
    msg.sample_rate = sample_rate;
    msg.channels = channels;
    msg.bit_depth = bit_depth;
    if (path) {
        strncpy(msg.path, path, sizeof(msg.path) - 1);
        msg.path[sizeof(msg.path) - 1] = '\0';
    } else {
        msg.path[0] = '\0';
    }

    int result = send_uart_message(WaveX::Protocol::MSG_SAMPLE_LOAD, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_sample_data(const uint8_t* data, size_t length) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // send_uart_message takes a uint16_t length. Without this check a length
    // of 65536+n truncates to n, passes the payload-size test inside, and
    // sends the wrong bytes while returning ESP_OK.
    if (length > WaveX::UartProtocol::UART_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "sample data too large (%u bytes)", (unsigned)length);
        return ESP_ERR_INVALID_SIZE;
    }

    int result =
        send_uart_message(WaveX::Protocol::MSG_SAMPLE_DATA, data, static_cast<uint16_t>(length));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_inst_op(uint32_t request_id,
                                 uint8_t slot,
                                 WaveX::Protocol::InstOpCode op,
                                 const char* path) {
    if (!s_initialized || s_suspended || !path || path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    WaveX::Protocol::InstOpMessage msg(request_id, slot, static_cast<uint8_t>(op), path);
    const int result = send_uart_message(WaveX::Protocol::MSG_INST_OP, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_mod_slot(uint32_t request_id,
                                  uint8_t instrument_slot,
                                  uint8_t mod_slot_index,
                                  uint8_t source,
                                  uint8_t dest,
                                  int16_t depth,
                                  uint8_t curve,
                                  uint8_t flags) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    WaveX::Protocol::InstOpMessage msg(
        request_id, instrument_slot, mod_slot_index, source, dest, depth, curve, flags);
    const int result = send_uart_message(WaveX::Protocol::MSG_INST_OP, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

void inter_mcu_handle_sample_stop_response(bool success) {
    ESP_LOGI("InterMCU", "inter_mcu_handle_sample_stop_response: success=%d", success ? 1 : 0);
    if (s_statistics) {
        s_statistics->invoke_sample_status_callback(
            0, 0, 0, 0, 0);  // sample_id=0, state=0 (stopped)
    } else {
        ESP_LOGE("InterMCU", "s_statistics is NULL in handle_sample_stop_response");
    }
}
