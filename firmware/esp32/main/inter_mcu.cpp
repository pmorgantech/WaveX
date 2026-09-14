#include "inter_mcu.h"

#include <string.h>

#include "../../shared/config/hardware_config.h"
#include "../../shared/config/link_config.h"
#include "../../shared/spi_protocol/protocol.h"
#include "../../shared/uart_protocol/uart_protocol.h"
#include "comm/listener_slot.h"
#include "comm/statistics.h"
#if WAVEX_SPI_LINK_ENABLED
#include "links/esp_spi_link.h"
#else
#include "links/esp_uart_link.h"
#endif

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

// Selected link state
static bool s_link_initialized = false;
static bool s_link_started = false;

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
// Pages register these with `this` and clear them in onExit; the link task
// invokes them. ListenerSlot makes the pair swap atomic and makes a clear
// block until any in-flight callback has returned - see listener_slot.h.
static WaveX::Comm::ListenerSlot<wavex_envelope_chunk_cb_t> s_envelope_chunk_listener;
static WaveX::Comm::ListenerSlot<wavex_inst_status_cb_t> s_inst_status_listener;

static int send_link_message(uint8_t msg_type, const void* payload, uint16_t len) {
    if (!s_link_initialized || !s_link_started) {
        ESP_LOGE(TAG, WAVEX_MCU_LINK_NAME " link not ready (msg=0x%02X)", msg_type);
        return -1;
    }

#if WAVEX_SPI_LINK_ENABLED
    int result = spi_link_send(msg_type, payload, len);
#else
    int result = uart_link_send(msg_type, payload, len);
#endif
    if (result < 0) {
        ESP_LOGE(TAG, WAVEX_MCU_LINK_NAME " send failed (msg=0x%02X)", msg_type);
    }
    return result;
}

esp_err_t inter_mcu_init(StatisticsManager& statistics) {
    if (s_initialized) {
        ESP_LOGI(TAG, "Inter-MCU communication already initialized");
        return ESP_OK;
    }

    s_statistics = &statistics;

    ESP_LOGI(TAG, "Initializing inter-MCU communication (" WAVEX_MCU_LINK_NAME ")...");

#if WAVEX_SPI_LINK_ENABLED
    esp_err_t ret = spi_link_init();
#else
    esp_err_t ret = uart_link_init();
#endif
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, WAVEX_MCU_LINK_NAME " link initialization failed");
        return ret;
    }
    s_link_initialized = true;
    ESP_LOGI(TAG, "Inter-MCU communication initialized successfully");
    s_initialized = true;

    return ESP_OK;
}

esp_err_t inter_mcu_start() {
    if (!s_initialized) {
        ESP_LOGE(TAG, "Inter-MCU communication not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting inter-MCU communication (" WAVEX_MCU_LINK_NAME ")...");

#if WAVEX_SPI_LINK_ENABLED
    esp_err_t ret = spi_link_start();
#else
    esp_err_t ret = uart_link_start();
#endif
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, WAVEX_MCU_LINK_NAME " link start failed");
        return ret;
    }
    s_link_started = true;

    ESP_LOGI(TAG, "Inter-MCU communication started successfully");

    return ESP_OK;
}

static WaveX::Comm::ListenerSlot<wavex_cv_cal_cb_t> s_cv_cal_listener;

esp_err_t inter_mcu_send_cv_cal_set(const WaveX::Protocol::CvCalMessage& cal) {
    if (!s_initialized || s_suspended) {
        return ESP_FAIL;
    }
    int result = send_link_message(WaveX::Protocol::MSG_CV_CAL_SET, &cal, sizeof(cal));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_cv_cal_get(uint8_t group) {
    if (!s_initialized || s_suspended) {
        return ESP_FAIL;
    }
    WaveX::Protocol::CvCalGetMessage msg(group);
    int result = send_link_message(WaveX::Protocol::MSG_CV_CAL_GET, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_cv_test(const WaveX::Protocol::CvTestMessage& test) {
    if (!s_initialized || s_suspended) {
        return ESP_FAIL;
    }
    int result = send_link_message(WaveX::Protocol::MSG_CV_TEST, &test, sizeof(test));
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

    int result = send_link_message(WaveX::Protocol::MSG_CONTROL_CHANGE, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

// The one place a NoteMessage is built. `addressed_channel` is already
// encoded - either a raw MIDI channel or NoteChannelForTrack(track) - so the
// addressing decision is made by the caller-facing wrappers below and never
// re-derived here.
static esp_err_t send_note(uint8_t msg_type,
                           uint8_t note,
                           uint8_t velocity,
                           uint8_t addressed_channel) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::NoteMessage msg;
    msg.note = note;
    msg.velocity = velocity;
    msg.channel = addressed_channel;
    msg.reserved = 0;

    int result = send_link_message(msg_type, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_note_on_midi(uint8_t note, uint8_t velocity, uint8_t channel) {
    // Raw MIDI channel, NOTE_ADDR_TRACK clear: the backend decides which
    // Tracks hear it. Masked rather than trusted so a caller that hands us a
    // channel with stray high bits cannot forge a Track address.
    return send_note(WaveX::Protocol::MSG_NOTE_ON,
                     note,
                     velocity,
                     channel & WaveX::Protocol::NOTE_ADDR_INDEX_MASK);
}

esp_err_t inter_mcu_send_note_off_midi(uint8_t note, uint8_t channel) {
    return send_note(
        WaveX::Protocol::MSG_NOTE_OFF, note, 0, channel & WaveX::Protocol::NOTE_ADDR_INDEX_MASK);
}

esp_err_t inter_mcu_send_note_on_track(uint8_t note, uint8_t velocity, uint8_t track) {
    return send_note(
        WaveX::Protocol::MSG_NOTE_ON, note, velocity, WaveX::Protocol::NoteChannelForTrack(track));
}

esp_err_t inter_mcu_send_note_off_track(uint8_t note, uint8_t track) {
    return send_note(
        WaveX::Protocol::MSG_NOTE_OFF, note, 0, WaveX::Protocol::NoteChannelForTrack(track));
}

esp_err_t inter_mcu_send_track_op(uint8_t op, uint8_t track, uint16_t value) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::TrackOpMessage msg(op, track, value);
    if (!WaveX::Protocol::IsValidTrackOp(msg))
        return ESP_ERR_INVALID_ARG;
    int result = send_link_message(WaveX::Protocol::MSG_TRACK_OP, &msg, sizeof(msg));
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

    int result = send_link_message(WaveX::Protocol::MSG_SAMPLE_CTRL, &msg, sizeof(msg));
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
    int result = send_link_message(WaveX::Protocol::MSG_ENVELOPE_REQ, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

namespace {
// Fixed cache of the records the Daisy has pushed. A record whose resident
// flag is clear (an unload) leaves the cache. Sized for what the pages
// actually display; the Sample Pool is far larger and is paged, not
// mirrored (track-and-patch-model.md §4).
constexpr size_t kMetaCacheSize = 32;
constexpr uint8_t kTrackBindingCount = WAVEX_MIX_TRACKS;
portMUX_TYPE s_meta_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::SampleMetadata s_meta[kMetaCacheSize];
bool s_meta_valid[kMetaCacheSize] = {};
size_t s_meta_next = 0;
uint16_t s_meta_newest_id = 0;

// The last Pool page. Written by the link RX task, read by the UI task.
portMUX_TYPE s_meta_page_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::SampleMetadata s_meta_page[WaveX::Protocol::MAX_SAMPLE_META_PAGE];
uint8_t s_meta_page_n = 0;
uint16_t s_meta_page_first = 0;
uint16_t s_meta_page_total = 0;
bool s_meta_page_valid = false;

portMUX_TYPE s_track_binding_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::TrackBindingMessage s_track_bindings[kTrackBindingCount];
bool s_track_binding_valid[kTrackBindingCount] = {};

// Bumped on the RX task, compared on the UI task; a page that keeps the
// value it last acted on can tell "nothing new" from "something arrived"
// without touching the caches or the link. Relaxed is enough: a reader that
// sees the bump then reads the caches under their own locks.
std::atomic<uint32_t> s_pool_revision{0};
std::atomic<uint32_t> s_cache_revision{0};

// One record into the per-id cache, no revision. The single-push entry point
// counts it as a Pool change; the page store, which files a whole page of
// records the Pool already had, must not.
void store_meta_record(const WaveX::Protocol::SampleMetadata& msg) {
    taskENTER_CRITICAL(&s_meta_lock);
    size_t slot = kMetaCacheSize;
    for (size_t i = 0; i < kMetaCacheSize; ++i) {
        if (s_meta_valid[i] && s_meta[i].sample_id == msg.sample_id) {
            slot = i;  // update in place, so an edit does not consume a slot
            break;
        }
    }
    if ((msg.flags & 1u) == 0) {
        // Not resident any more: forget it rather than store a ghost.
        if (slot != kMetaCacheSize) {
            s_meta_valid[slot] = false;
        }
        if (s_meta_newest_id == msg.sample_id) {
            s_meta_newest_id = 0;
        }
        taskEXIT_CRITICAL(&s_meta_lock);
        return;
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
}  // namespace

void inter_mcu_store_sample_meta(const WaveX::Protocol::SampleMetadata& msg) {
    store_meta_record(msg);
    s_pool_revision.fetch_add(1, std::memory_order_relaxed);
    s_cache_revision.fetch_add(1, std::memory_order_relaxed);
}

uint32_t inter_mcu_sample_pool_revision() {
    return s_pool_revision.load(std::memory_order_relaxed);
}

uint32_t inter_mcu_sample_cache_revision() {
    return s_cache_revision.load(std::memory_order_relaxed);
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

bool inter_mcu_find_sample_meta_by_name(const char* path, WaveX::Protocol::SampleMetadata* out) {
    if (!path || !path[0] || !out) {
        return false;
    }
    constexpr size_t kKept = WaveX::Protocol::FILE_NAME_MAX - 1;
    const size_t path_len = strnlen(path, kKept + 1);
    bool found = false;
    taskENTER_CRITICAL(&s_meta_lock);
    for (size_t i = 0; i < kMetaCacheSize; ++i) {
        if (!s_meta_valid[i]) {
            continue;
        }
        const char* name = s_meta[i].name;
        const size_t name_len = strnlen(name, kKept);
        // A name that fills the field was cut at kKept; a path that long can
        // only be compared on what survived.
        const bool match = (name_len == kKept && path_len >= kKept)
                               ? (strncmp(name, path, kKept) == 0)
                               : (name_len == path_len && strncmp(name, path, kKept) == 0);
        if (match) {
            *out = s_meta[i];
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_meta_lock);
    return found;
}

size_t inter_mcu_sample_meta_snapshot(WaveX::Protocol::SampleMetadata* out, size_t max) {
    size_t n = 0;
    taskENTER_CRITICAL(&s_meta_lock);
    for (size_t i = 0; i < kMetaCacheSize && n < max; ++i) {
        if (s_meta_valid[i]) {
            out[n++] = s_meta[i];
        }
    }
    taskEXIT_CRITICAL(&s_meta_lock);
    return n;
}

esp_err_t inter_mcu_request_sample_meta_page(uint16_t first, uint8_t count) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    WaveX::Protocol::SampleMetaPageReqMessage msg(first, count);
    const int result =
        send_link_message(WaveX::Protocol::MSG_SAMPLE_META_PAGE_REQ, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

void inter_mcu_store_sample_meta_page(const WaveX::Protocol::SampleMetaPageHeader& header,
                                      const uint8_t* records) {
    const uint8_t n = header.n > WaveX::Protocol::MAX_SAMPLE_META_PAGE
                          ? WaveX::Protocol::MAX_SAMPLE_META_PAGE
                          : header.n;
    taskENTER_CRITICAL(&s_meta_page_lock);
    memcpy(s_meta_page, records, n * sizeof(WaveX::Protocol::SampleMetadata));
    s_meta_page_n = n;
    s_meta_page_first = header.first;
    s_meta_page_total = header.total;
    s_meta_page_valid = true;
    taskEXIT_CRITICAL(&s_meta_page_lock);
    // The per-id cache sees them too, so a page a list showed can be looked
    // up by id afterwards (detail views, the picker prompt).
    for (uint8_t i = 0; i < n; ++i) {
        WaveX::Protocol::SampleMetadata m;
        memcpy(&m, records + i * sizeof(m), sizeof(m));
        store_meta_record(m);
    }
    s_cache_revision.fetch_add(1, std::memory_order_relaxed);
}

size_t inter_mcu_get_sample_meta_page(WaveX::Protocol::SampleMetadata* out,
                                      size_t max,
                                      uint16_t* total,
                                      uint16_t* first) {
    size_t n = 0;
    taskENTER_CRITICAL(&s_meta_page_lock);
    if (s_meta_page_valid) {
        n = s_meta_page_n < max ? s_meta_page_n : max;
        for (size_t i = 0; i < n; ++i) {
            out[i] = s_meta_page[i];
        }
        if (total) {
            *total = s_meta_page_total;
        }
        if (first) {
            *first = s_meta_page_first;
        }
    } else {
        if (total) {
            *total = 0;
        }
        if (first) {
            *first = 0;
        }
    }
    taskEXIT_CRITICAL(&s_meta_page_lock);
    return n;
}

esp_err_t inter_mcu_request_sample_meta(uint16_t sample_id) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    WaveX::Protocol::SampleMetaReqMessage msg(sample_id);
    int result = send_link_message(WaveX::Protocol::MSG_SAMPLE_META_REQ, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_request_track_binding(uint8_t track) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    if (track != 0xFF && track >= kTrackBindingCount) {
        return ESP_ERR_INVALID_ARG;
    }
    WaveX::Protocol::TrackBindingReqMessage msg(track);
    const int result = send_link_message(WaveX::Protocol::MSG_TRACK_BINDING_REQ, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

void inter_mcu_store_track_binding(const WaveX::Protocol::TrackBindingMessage& msg) {
    if (msg.track >= kTrackBindingCount) {
        return;
    }
    // A binding that differs from the cached one is a Pool change as well:
    // the record's used_by moved, and the backend does not push the record
    // for a bind. A reply that only confirms the cache (the all-Tracks
    // request a page makes on entry) is not.
    bool changed = true;
    taskENTER_CRITICAL(&s_track_binding_lock);
    if (s_track_binding_valid[msg.track]) {
        const WaveX::Protocol::TrackBindingMessage& was = s_track_bindings[msg.track];
        changed = was.state != msg.state || was.sample_id != msg.sample_id ||
                  strncmp(was.name, msg.name, sizeof(was.name)) != 0;
    }
    s_track_bindings[msg.track] = msg;
    // The name is a fixed-width wire field, not a guaranteed C string: a name
    // that exactly fills it carries no terminator. Terminate once here so
    // every UI reader can treat the cached copy as an ordinary string.
    s_track_bindings[msg.track].name[sizeof(s_track_bindings[msg.track].name) - 1] = '\0';
    s_track_binding_valid[msg.track] = true;
    taskEXIT_CRITICAL(&s_track_binding_lock);
    if (changed) {
        s_pool_revision.fetch_add(1, std::memory_order_relaxed);
    }
    s_cache_revision.fetch_add(1, std::memory_order_relaxed);
}

bool inter_mcu_get_track_binding(uint8_t track, WaveX::Protocol::TrackBindingMessage* out) {
    if (!out || track >= kTrackBindingCount) {
        return false;
    }
    taskENTER_CRITICAL(&s_track_binding_lock);
    const bool found = s_track_binding_valid[track];
    if (found) {
        *out = s_track_bindings[track];
    }
    taskEXIT_CRITICAL(&s_track_binding_lock);
    return found;
}

esp_err_t inter_mcu_send_sample_select(uint16_t sample_id, uint8_t slot) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    WaveX::Protocol::SampleSelectMessage msg(sample_id, slot);
    int result = send_link_message(WaveX::Protocol::MSG_SAMPLE_SELECT, &msg, sizeof(msg));
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
    int result = send_link_message(WaveX::Protocol::MSG_SAMPLE_UNLOAD, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_sample_edit(uint16_t sample_id,
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
    WaveX::Protocol::SampleEditMessage msg(sample_id,
                                           loop_enabled ? 1 : 0,
                                           gain_db_x10,
                                           start_frame,
                                           end_frame,
                                           loop_start,
                                           loop_end,
                                           fade_in_ms,
                                           fade_out_ms);
    int result = send_link_message(WaveX::Protocol::MSG_SAMPLE_EDIT_SET, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_diag_subscribe(bool enable, uint8_t interval_hz) {
    if (!s_initialized || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }
    WaveX::Protocol::DiagSubscribeMessage msg(enable ? 1 : 0, interval_hz);
    int result = send_link_message(WaveX::Protocol::MSG_DIAG_SUBSCRIBE, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

namespace {
// Written by the link RX task, read by the UI task. A portMUX critical
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

    // The public API has no synchronous busy/admission query.
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
    int result = send_link_message(WaveX::Protocol::MSG_STATUS_REQUEST, &req, sizeof(req));
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
// second pass - under taskENTER_CRITICAL, on the link RX task. That disables
// interrupts on the core for the duration, and this runs on the path every
// sample-status message takes, including the burst a load emits. Long critical
// sections are exactly what docs/esp32p4_coding_guide.md and the esp32p4 skill
// warn against ("short portMUX_TYPE critical sections"), and a section held too
// long is how the interrupt watchdog fires.
//
// Reading s_meta[] unlocked is safe here: this task is the only writer, and a
// concurrent UI-task reader is also only reading. The worst a race can do is
// mark a slot that changed in between, which the next status corrects.
// Returns whether a record left the cache: the one Pool change nothing else
// announces (an eviction notifies nobody), so the caller counts it as one.
bool prune_sample_meta_to(const wavex_sample_mem_status_t& status) {
    if (status.sample_count >= WAVEX_SAMPLE_STATUS_MAX_ENTRIES) {
        return false;  // possibly truncated: cannot prove absence
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
    bool dropped = false;
    taskENTER_CRITICAL(&s_meta_lock);
    for (size_t i = 0; i < kMetaCacheSize; ++i) {
        if (drop[i]) {
            s_meta_valid[i] = false;
            dropped = true;
        }
    }
    if (!newest_live) {
        s_meta_newest_id = new_newest;
    }
    taskEXIT_CRITICAL(&s_meta_lock);
    return dropped;
}

}  // namespace

void inter_mcu_update_sample_mem_status(const wavex_sample_mem_status_t& status) {
    taskENTER_CRITICAL(&s_sample_mem_lock);
    s_sample_mem_status = status;
    taskEXIT_CRITICAL(&s_sample_mem_lock);
    // Outside the lock above: this takes s_meta_lock, and nesting the two
    // spinlocks would create the only lock ordering in this file.
    if (prune_sample_meta_to(status)) {
        s_pool_revision.fetch_add(1, std::memory_order_relaxed);
    }
    s_cache_revision.fetch_add(1, std::memory_order_relaxed);
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

bool inter_mcu_backend_link_alive(void) {
    wavex_backend_heartbeat_t hb;
    memset(&hb, 0, sizeof(hb));
    inter_mcu_get_backend_heartbeat(&hb);
    if (!hb.valid || hb.last_rx_ms == 0) {
        return false;
    }
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    // Unsigned subtraction, so a wrapped timer reads as "just heard from it"
    // for one tick rather than as a dead link for 49 days.
    return (now_ms - hb.last_rx_ms) < WAVEX_LINK_STALE_MS;
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

esp_err_t inter_mcu_send_browse_req(const char* path,
                                    uint8_t start_index,
                                    WaveX::Protocol::BrowseFilter filter) {
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

    uint8_t payload[WaveX::Protocol::BROWSE_DIRECTORY_PATH_MAX + 2]{};
    const size_t payload_len =
        WaveX::Protocol::EncodeBrowseRequest(payload, sizeof(payload), path, start_index, filter);
    if (!payload_len)
        return ESP_ERR_INVALID_ARG;

    int result = send_link_message(WaveX::Protocol::MSG_BROWSE_REQ, payload, (uint16_t)payload_len);

    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_sample_play_index_req(uint32_t file_index, uint16_t loop_gap_ms) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    WaveX::Protocol::SamplePlayIndexMessage msg;
    msg.index = file_index;
    msg.loop_gap_ms = loop_gap_ms;

    int result = send_link_message(WaveX::Protocol::MSG_SAMPLE_PLAY_INDEX_REQ, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_sample_audition(uint16_t sample_id) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    WaveX::Protocol::SampleAuditionMessage msg(sample_id);
    return send_link_message(WaveX::Protocol::MSG_SAMPLE_AUDITION, &msg, sizeof(msg)) >= 0
               ? ESP_OK
               : ESP_FAIL;
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

    int result = send_link_message(WaveX::Protocol::MSG_SAMPLE_STOP_REQ, &msg, sizeof(msg));
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

    int result = send_link_message(WaveX::Protocol::MSG_SAMPLE_LOAD, &msg, sizeof(msg));
    return result >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t inter_mcu_send_sample_data(const uint8_t* data, size_t length) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // send_link_message takes a uint16_t length. Without this check a length
    // of 65536+n truncates to n, passes the payload-size test inside, and
    // sends the wrong bytes while returning ESP_OK.
    if (length > WaveX::UartProtocol::UART_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "sample data too large (%u bytes)", (unsigned)length);
        return ESP_ERR_INVALID_SIZE;
    }

    int result =
        send_link_message(WaveX::Protocol::MSG_SAMPLE_DATA, data, static_cast<uint16_t>(length));
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
    const int result = send_link_message(WaveX::Protocol::MSG_INST_OP, &msg, sizeof(msg));
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
    const int result = send_link_message(WaveX::Protocol::MSG_INST_OP, &msg, sizeof(msg));
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

namespace {
portMUX_TYPE s_seq_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::SeqPatternSyncMessage s_seq_page;
WaveX::Protocol::SeqPlayheadMessage s_seq_playhead;
bool s_seq_page_valid = false;
bool s_seq_playhead_valid = false;
}  // namespace

esp_err_t inter_mcu_send_mix_op(uint8_t op, uint8_t track, uint16_t value) {
    const WaveX::Protocol::MixOpMessage message(op, track, value);
    return send_link_message(WaveX::Protocol::MSG_MIX_OP, &message, sizeof(message)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t inter_mcu_send_seq_transport(const WaveX::Protocol::SeqTransportMessage& message) {
    return send_link_message(WaveX::Protocol::MSG_SEQ_TRANSPORT, &message, sizeof(message)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t inter_mcu_send_seq_pattern_op(const WaveX::Protocol::SeqPatternOpMessage& message) {
    return send_link_message(WaveX::Protocol::MSG_SEQ_PATTERN_OP, &message, sizeof(message)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t inter_mcu_request_seq_page(const WaveX::Protocol::SeqPatternRequestMessage& request) {
    if (!WaveX::Protocol::IsValidSeqPatternRequest(request))
        return ESP_ERR_INVALID_ARG;
    return send_link_message(WaveX::Protocol::MSG_SEQ_PATTERN_SYNC, &request, sizeof(request)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}

void inter_mcu_store_seq_page(const WaveX::Protocol::SeqPatternSyncMessage& page) {
    taskENTER_CRITICAL(&s_seq_snapshot_lock);
    s_seq_page = page;
    s_seq_page_valid = true;
    taskEXIT_CRITICAL(&s_seq_snapshot_lock);
}

void inter_mcu_store_seq_playhead(const WaveX::Protocol::SeqPlayheadMessage& playhead) {
    taskENTER_CRITICAL(&s_seq_snapshot_lock);
    s_seq_playhead = playhead;
    s_seq_playhead_valid = true;
    taskEXIT_CRITICAL(&s_seq_snapshot_lock);
}

bool inter_mcu_get_seq_page(WaveX::Protocol::SeqPatternSyncMessage* out) {
    if (!out)
        return false;
    taskENTER_CRITICAL(&s_seq_snapshot_lock);
    const bool valid = s_seq_page_valid;
    if (valid)
        *out = s_seq_page;
    taskEXIT_CRITICAL(&s_seq_snapshot_lock);
    return valid;
}

bool inter_mcu_get_seq_playhead(WaveX::Protocol::SeqPlayheadMessage* out) {
    if (!out)
        return false;
    taskENTER_CRITICAL(&s_seq_snapshot_lock);
    const bool valid = s_seq_playhead_valid;
    if (valid)
        *out = s_seq_playhead;
    taskEXIT_CRITICAL(&s_seq_snapshot_lock);
    return valid;
}

namespace {
portMUX_TYPE s_instrument_map_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::InstZoneSyncMessage s_instrument_map;
bool s_instrument_map_valid = false;
}  // namespace
esp_err_t inter_mcu_send_instrument_edit(const WaveX::Protocol::InstOpMessage& request) {
    if (request.slot >= 16 || !request.request_id)
        return ESP_ERR_INVALID_ARG;
    return send_link_message(WaveX::Protocol::MSG_INST_OP, &request, sizeof(request)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}
void inter_mcu_store_instrument_map(const WaveX::Protocol::InstZoneSyncMessage& map) {
    taskENTER_CRITICAL(&s_instrument_map_lock);
    s_instrument_map = map;
    s_instrument_map_valid = true;
    taskEXIT_CRITICAL(&s_instrument_map_lock);
}
bool inter_mcu_get_instrument_map(WaveX::Protocol::InstZoneSyncMessage* out) {
    if (!out)
        return false;
    taskENTER_CRITICAL(&s_instrument_map_lock);
    const bool valid = s_instrument_map_valid;
    if (valid)
        *out = s_instrument_map;
    taskEXIT_CRITICAL(&s_instrument_map_lock);
    return valid;
}

namespace {
portMUX_TYPE s_seq_file_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::SeqFileStatusMessage s_seq_file_status;
bool s_seq_file_valid = false;
}  // namespace
esp_err_t inter_mcu_send_seq_file_op(const WaveX::Protocol::SeqFileOpMessage& request) {
    if (!WaveX::Protocol::IsValidSeqFileOp(request))
        return ESP_ERR_INVALID_ARG;
    return send_link_message(WaveX::Protocol::MSG_SEQ_FILE_OP, &request, sizeof(request)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}
void inter_mcu_store_seq_file_status(const WaveX::Protocol::SeqFileStatusMessage& status) {
    taskENTER_CRITICAL(&s_seq_file_lock);
    s_seq_file_status = status;
    s_seq_file_valid = true;
    taskEXIT_CRITICAL(&s_seq_file_lock);
}
bool inter_mcu_get_seq_file_status(WaveX::Protocol::SeqFileStatusMessage* out) {
    if (!out)
        return false;
    taskENTER_CRITICAL(&s_seq_file_lock);
    const bool valid = s_seq_file_valid;
    if (valid)
        *out = s_seq_file_status;
    taskEXIT_CRITICAL(&s_seq_file_lock);
    return valid;
}

namespace {
portMUX_TYPE s_oscillator_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::InstOscSyncMessage s_oscillator;
bool s_oscillator_valid = false;
}  // namespace
esp_err_t inter_mcu_send_oscillator(const WaveX::Protocol::InstOscOpMessage& request) {
    if (!WaveX::Protocol::IsValidInstOscOp(request))
        return ESP_ERR_INVALID_ARG;
    return send_link_message(WaveX::Protocol::MSG_INST_OSC_OP, &request, sizeof(request)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}
void inter_mcu_store_oscillator(const WaveX::Protocol::InstOscSyncMessage& state) {
    taskENTER_CRITICAL(&s_oscillator_lock);
    s_oscillator = state;
    s_oscillator_valid = true;
    taskEXIT_CRITICAL(&s_oscillator_lock);
}
bool inter_mcu_get_oscillator(WaveX::Protocol::InstOscSyncMessage* out) {
    if (!out)
        return false;
    taskENTER_CRITICAL(&s_oscillator_lock);
    const bool valid = s_oscillator_valid;
    if (valid)
        *out = s_oscillator;
    taskEXIT_CRITICAL(&s_oscillator_lock);
    return valid;
}

namespace {
portMUX_TYPE s_instrument_edit_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::InstEditSyncMessage s_instrument_edit;
bool s_instrument_edit_valid = false;
}  // namespace
esp_err_t inter_mcu_send_instrument_edit(const WaveX::Protocol::InstEditOpMessage& request) {
    if (!WaveX::Protocol::IsValidInstEditOp(request))
        return ESP_ERR_INVALID_ARG;
#if WAVEX_LINK_LATENCY_PROFILE_ENABLED && defined(ESP_PLATFORM)
    const int64_t started_us = esp_timer_get_time();
#endif
    const int result =
        send_link_message(WaveX::Protocol::MSG_INST_EDIT_OP, &request, sizeof(request));
#if WAVEX_LINK_LATENCY_PROFILE_ENABLED && defined(ESP_PLATFORM)
    // Print after admission; the timestamp precedes queueing. Correlate by
    // request ID/track offline, so the UI and RX tasks share no profiling state.
    ESP_LOGI(TAG,
             "LINK_LATENCY TX us=%lld id=%lu track=%u op=%u result=%d",
             static_cast<long long>(started_us),
             static_cast<unsigned long>(request.request_id),
             request.track,
             request.op,
             result);
#endif
    return result >= 0 ? ESP_OK : ESP_FAIL;
}
void inter_mcu_store_instrument_edit(const WaveX::Protocol::InstEditSyncMessage& state) {
#if WAVEX_LINK_LATENCY_PROFILE_ENABLED && defined(ESP_PLATFORM)
    // This is a control round trip through backend processing, not one-way
    // wire latency or time until the audio callback applies the value.
    const int64_t received_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "LINK_LATENCY RX us=%lld id=%lu track=%u completed=%lu valid=%u busy=%u error=%u",
             static_cast<long long>(received_us),
             static_cast<unsigned long>(state.request_id),
             state.track,
             static_cast<unsigned long>(state.completed_request_id),
             state.valid,
             state.busy,
             state.error);
#endif
    taskENTER_CRITICAL(&s_instrument_edit_lock);
    s_instrument_edit = state;
    s_instrument_edit_valid = true;
    taskEXIT_CRITICAL(&s_instrument_edit_lock);
}
bool inter_mcu_get_instrument_edit(WaveX::Protocol::InstEditSyncMessage* out) {
    if (!out)
        return false;
    taskENTER_CRITICAL(&s_instrument_edit_lock);
    const bool valid = s_instrument_edit_valid;
    if (valid)
        *out = s_instrument_edit;
    taskEXIT_CRITICAL(&s_instrument_edit_lock);
    return valid;
}

namespace {
portMUX_TYPE s_modulator_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::InstModSyncMessage s_modulator;
bool s_modulator_valid = false;
}  // namespace
esp_err_t inter_mcu_send_modulator(const WaveX::Protocol::InstModOpMessage& request) {
    if (!WaveX::Protocol::IsValidInstModOp(request))
        return ESP_ERR_INVALID_ARG;
    return send_link_message(WaveX::Protocol::MSG_INST_MOD_OP, &request, sizeof(request)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}
void inter_mcu_store_modulator(const WaveX::Protocol::InstModSyncMessage& state) {
    taskENTER_CRITICAL(&s_modulator_lock);
    s_modulator = state;
    s_modulator_valid = true;
    taskEXIT_CRITICAL(&s_modulator_lock);
}
bool inter_mcu_get_modulator(WaveX::Protocol::InstModSyncMessage* out) {
    if (!out)
        return false;
    taskENTER_CRITICAL(&s_modulator_lock);
    const bool valid = s_modulator_valid;
    if (valid)
        *out = s_modulator;
    taskEXIT_CRITICAL(&s_modulator_lock);
    return valid;
}

namespace {
portMUX_TYPE s_instrument_lfo_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::InstLfoSyncMessage s_instrument_lfo;
bool s_instrument_lfo_valid = false;
}  // namespace
esp_err_t inter_mcu_send_instrument_lfo(const WaveX::Protocol::InstLfoOpMessage& request) {
    if (!WaveX::Protocol::IsValidInstLfoOp(request))
        return ESP_ERR_INVALID_ARG;
    return send_link_message(WaveX::Protocol::MSG_INST_LFO_OP, &request, sizeof(request)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}
void inter_mcu_store_instrument_lfo(const WaveX::Protocol::InstLfoSyncMessage& state) {
    taskENTER_CRITICAL(&s_instrument_lfo_lock);
    s_instrument_lfo = state;
    s_instrument_lfo_valid = true;
    taskEXIT_CRITICAL(&s_instrument_lfo_lock);
}
bool inter_mcu_get_instrument_lfo(WaveX::Protocol::InstLfoSyncMessage* out) {
    if (!out)
        return false;
    taskENTER_CRITICAL(&s_instrument_lfo_lock);
    const bool valid = s_instrument_lfo_valid;
    if (valid)
        *out = s_instrument_lfo;
    taskEXIT_CRITICAL(&s_instrument_lfo_lock);
    return valid;
}

namespace {
portMUX_TYPE s_key_map_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::InstKeyMapSyncMessage s_key_map;
bool s_key_map_valid = false;
}  // namespace
esp_err_t inter_mcu_send_key_map(const WaveX::Protocol::InstKeyMapOpMessage& request) {
    if (!WaveX::Protocol::IsValidKeyMapOp(request))
        return ESP_ERR_INVALID_ARG;
    return send_link_message(WaveX::Protocol::MSG_INST_KEY_MAP_OP, &request, sizeof(request)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}
void inter_mcu_store_key_map(const WaveX::Protocol::InstKeyMapSyncMessage& state) {
    taskENTER_CRITICAL(&s_key_map_lock);
    s_key_map = state;
    s_key_map_valid = true;
    taskEXIT_CRITICAL(&s_key_map_lock);
}
bool inter_mcu_get_key_map(WaveX::Protocol::InstKeyMapSyncMessage* out) {
    if (!out)
        return false;
    taskENTER_CRITICAL(&s_key_map_lock);
    const bool valid = s_key_map_valid;
    if (valid)
        *out = s_key_map;
    taskEXIT_CRITICAL(&s_key_map_lock);
    return valid;
}

namespace {
portMUX_TYPE s_pad_sound_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::InstPadSoundSyncMessage s_pad_sound;
bool s_pad_sound_valid = false;
}  // namespace
esp_err_t inter_mcu_send_pad_sound(const WaveX::Protocol::InstPadSoundOpMessage& request) {
    if (!WaveX::Protocol::IsValidPadSoundOp(request))
        return ESP_ERR_INVALID_ARG;
    return send_link_message(WaveX::Protocol::MSG_INST_PAD_SOUND_OP, &request, sizeof(request)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}
void inter_mcu_store_pad_sound(const WaveX::Protocol::InstPadSoundSyncMessage& state) {
    taskENTER_CRITICAL(&s_pad_sound_lock);
    s_pad_sound = state;
    s_pad_sound_valid = true;
    taskEXIT_CRITICAL(&s_pad_sound_lock);
}
bool inter_mcu_get_pad_sound(WaveX::Protocol::InstPadSoundSyncMessage* out) {
    if (!out)
        return false;
    taskENTER_CRITICAL(&s_pad_sound_lock);
    const bool valid = s_pad_sound_valid;
    if (valid)
        *out = s_pad_sound;
    taskEXIT_CRITICAL(&s_pad_sound_lock);
    return valid;
}

namespace {
portMUX_TYPE s_mix_state_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::MixStateMessage s_mix_state;
bool s_mix_state_valid = false;
portMUX_TYPE s_track_state_lock = portMUX_INITIALIZER_UNLOCKED;
WaveX::Protocol::TrackStateMessage s_track_state;
bool s_track_state_valid = false;
}  // namespace
esp_err_t inter_mcu_set_track_mix(const WaveX::Protocol::MixOpMessage& message) {
    using namespace WaveX::Protocol;
    if (message.track >= 16 || (message.op != MIX_OP_SET_GAIN && message.op != MIX_OP_SET_PAN) ||
        (message.op == MIX_OP_SET_GAIN && message.value > 6600))
        return ESP_ERR_INVALID_ARG;
    return send_link_message(MSG_MIX_OP, &message, sizeof(message)) >= 0 ? ESP_OK : ESP_FAIL;
}
esp_err_t inter_mcu_request_mix_state(const WaveX::Protocol::MixStateRequest& request) {
    if (!request.request_id || request.track >= 16)
        return ESP_ERR_INVALID_ARG;
    return send_link_message(WaveX::Protocol::MSG_MIX_STATE_REQ, &request, sizeof(request)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}
void inter_mcu_store_mix_state(const WaveX::Protocol::MixStateMessage& state) {
    if (!WaveX::Protocol::IsValidMixState(state))
        return;
    taskENTER_CRITICAL(&s_mix_state_lock);
    s_mix_state = state;
    s_mix_state_valid = true;
    taskEXIT_CRITICAL(&s_mix_state_lock);
}
bool inter_mcu_get_mix_state(WaveX::Protocol::MixStateMessage* out) {
    if (!out)
        return false;
    taskENTER_CRITICAL(&s_mix_state_lock);
    const bool valid = s_mix_state_valid;
    if (valid)
        *out = s_mix_state;
    taskEXIT_CRITICAL(&s_mix_state_lock);
    return valid;
}
esp_err_t inter_mcu_request_track_state(const WaveX::Protocol::TrackStateRequest& request) {
    if (!request.request_id || request.track >= 16)
        return ESP_ERR_INVALID_ARG;
    return send_link_message(WaveX::Protocol::MSG_TRACK_STATE_REQ, &request, sizeof(request)) >= 0
               ? ESP_OK
               : ESP_FAIL;
}
void inter_mcu_store_track_state(const WaveX::Protocol::TrackStateMessage& state) {
    taskENTER_CRITICAL(&s_track_state_lock);
    s_track_state = state;
    s_track_state_valid = true;
    taskEXIT_CRITICAL(&s_track_state_lock);
}
bool inter_mcu_get_track_state(WaveX::Protocol::TrackStateMessage* out) {
    if (!out)
        return false;
    taskENTER_CRITICAL(&s_track_state_lock);
    const bool valid = s_track_state_valid;
    if (valid)
        *out = s_track_state;
    taskEXIT_CRITICAL(&s_track_state_lock);
    return valid;
}
