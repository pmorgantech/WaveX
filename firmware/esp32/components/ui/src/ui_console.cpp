// ESP32 debug console: one line reader, two grammars, acknowledged verbs.
// See ui_console.h and docs/features/debug-harness-and-hil.md.
#include "ui/ui_console.h"

#if WAVEX_DEBUG_HARNESS_ENABLED

#include "config/hardware_config.h"
#include "debug/console_command.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "inter_mcu.h"
#include "lvgl.h"
#include "ui/current_track.h"
#include "ui/input_dispatcher.h"
#include "ui/panel_key.h"
#include "ui/ui_navigator.h"
#include "ui/ui_screenshot.h"
#include "ui/ui_softkey.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

const char* TAG = "UI_CONSOLE";

// Console UART. Commands arrive on the same port the logs leave on, so the
// host drives everything through one tty.
constexpr uart_port_t kUart = UART_NUM_0;

using namespace WaveX::Debug;

// ---------------------------------------------------------------------------
// Synthetic touch: a second LVGL pointer indev fed from a queue (§2 of the
// design doc). The GT911 indev is untouched; this one reports "released"
// unless a test is driving it. A tap must hold PRESSED across several read
// cycles - LVGL derives click from press state seen on successive reads.
// ---------------------------------------------------------------------------
struct TouchStep {
    int16_t x;
    int16_t y;
    uint8_t pressed;
};
constexpr int kTapHoldReads = 3;
QueueHandle_t s_touch_q = nullptr;
lv_indev_t* s_touch_indev = nullptr;
TouchStep s_touch_now{0, 0, 0};

void touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    // lvgl_port task context, never an ISR: a zero-timeout receive is the
    // right primitive. One step per read so a queued tap plays out over
    // real read cycles rather than collapsing into a single blip.
    TouchStep st;
    if (s_touch_q && xQueueReceive(s_touch_q, &st, 0) == pdTRUE) {
        s_touch_now = st;
    }
    // Hosts speak screen (rotated) coordinates - the ones STATE reports for
    // softkeys and tabs. LVGL applies lv_display_rotate_point() to every
    // pointer sample, so hand it the raw panel point that rotates onto the
    // requested screen point.
    lv_display_t* disp = lv_indev_get_display(indev);
    int32_t x = s_touch_now.x;
    int32_t y = s_touch_now.y;
    if (disp) {
        const int32_t hor = lv_display_get_horizontal_resolution(disp);  // rotated
        const int32_t ver = lv_display_get_vertical_resolution(disp);
        switch (lv_display_get_rotation(disp)) {
            case LV_DISPLAY_ROTATION_90:
                x = s_touch_now.y;
                y = hor - 1 - s_touch_now.x;
                break;
            case LV_DISPLAY_ROTATION_180:
                x = hor - 1 - s_touch_now.x;
                y = ver - 1 - s_touch_now.y;
                break;
            case LV_DISPLAY_ROTATION_270:
                x = ver - 1 - s_touch_now.y;
                y = s_touch_now.x;
                break;
            default:
                break;
        }
    }
    data->point.x = x;
    data->point.y = y;
    data->state = s_touch_now.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool touch_enqueue(int16_t x, int16_t y, bool pressed) {
    TouchStep st{x, y, static_cast<uint8_t>(pressed ? 1 : 0)};
    return s_touch_q && xQueueSend(s_touch_q, &st, 0) == pdTRUE;
}

// ---------------------------------------------------------------------------
// UI-task mailbox: verbs that touch LVGL are served by wavex_console_poll().
// One slot; a second request while one is in flight is answered ERR busy
// rather than queued, so the host always knows which reply is whose.
// ---------------------------------------------------------------------------
enum class ReqState : uint8_t { Idle, Pending, Done };
struct UiRequest {
    int32_t seq = 0;
    char verb[kMaxVerbBytes] = {};
    char args[160] = {};
};
std::atomic<ReqState> s_req_state{ReqState::Idle};
UiRequest s_req;
char s_reply[640];

// ---------------------------------------------------------------------------
// Console-task verbs
// ---------------------------------------------------------------------------

// Applies a "LOG" command. Two level stores exist on the ESP32 and BOTH gate
// WAVEX_LOGx output: the shared module table and IDF's per-tag level
// (default INFO). A module hit therefore also mirrors into
// esp_log_level_set("WAVEX-<MODULE>", ...); an unmatched name is applied as
// a verbatim IDF tag (UI_NAVIGATOR, packet_router, ...), which is how the
// 400+ plain ESP_LOGx call sites are tuned. Replies go to the console so
// wavex_log.py can tail them from the logger's file.
bool handle_log_command(const char* cmd) {
    using namespace WaveX::Log;

    if (cmd[0] == '?' && cmd[1] == '\0') {
        for (size_t m = 0; m < kModuleCount; ++m) {
            printf("WAVEX-LOG: %s=%s\n",
                   kModuleNames[m],
                   kLevelNames[GetLevel(static_cast<Module>(m))]);
        }
        return true;
    }

    char name[32];
    char lvl_tok[8];
    const char* p = cmd;
    NextWord(&p, name, sizeof(name));
    NextWord(&p, lvl_tok, sizeof(lvl_tok));

    const int level = ParseLevelToken(lvl_tok);
    if (name[0] == '\0' || level < 0) {
        printf(
            "WAVEX-LOG: bad command '%s' - usage: WAVEX-LOG "
            "<MODULE|tag|*> <OFF|ERROR|WARN|INFO|DEBUG|TRACE|0-5>\n",
            cmd);
        return false;
    }
    const auto esp_level = static_cast<esp_log_level_t>(level);

    if (name[0] == '*' && name[1] == '\0') {
        SetAllLevels(static_cast<uint8_t>(level));
        esp_log_level_set("*", esp_level);
        printf("WAVEX-LOG: *=%s\n", kLevelNames[level]);
        return true;
    }
    if (SetLevelByName(name, static_cast<uint8_t>(level))) {
        char tag[40] = "WAVEX-";
        snprintf(tag + 6, sizeof(tag) - 6, "%s", name);
        esp_log_level_set(tag, esp_level);
        printf("WAVEX-LOG: %s=%s\n", tag + 6, kLevelNames[level]);
        return true;
    }
    // NextWord upper-cased the name; IDF tags are case-sensitive, so take
    // the tag verbatim from the original command instead.
    char raw[32];
    size_t n = 0;
    const char* q = cmd;
    while (*q == ' ')
        ++q;
    while (q[n] && q[n] != ' ' && n + 1 < sizeof(raw)) {
        raw[n] = q[n];
        ++n;
    }
    raw[n] = '\0';
    esp_log_level_set(raw, esp_level);
    printf("WAVEX-LOG: tag %s=%s\n", raw, kLevelNames[level]);
    return true;
}

bool post_input(wavex_ui::InputType type, uint8_t source, int16_t delta) {
    wavex_ui::InputEvent evt{};
    evt.type = type;
    evt.source_id = source;
    evt.delta = delta;
    evt.timestamp_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    return wavex_ui::InputDispatcher::instance().post(evt);
}

void reply_ok(int32_t seq) {
    char out[64];
    FormatOk(seq, out, sizeof(out));
    printf("%s\n", out);
}

void reply_err(int32_t seq, const char* reason) {
    char out[96];
    FormatErr(seq, reason, out, sizeof(out));
    printf("%s\n", out);
}

// Hands a verb to the UI task and waits for its reply. The console task is
// dedicated, so blocking it here costs nothing; the bound keeps a wedged UI
// from silencing the console forever.
void run_on_ui_task(const Command& c) {
    if (s_req_state.load(std::memory_order_acquire) != ReqState::Idle) {
        reply_err(c.seq, "busy");
        return;
    }
    // Fill the slot before publishing it: this task is the only writer, and
    // the release-store below is what makes the fields visible to the UI task.
    s_req.seq = c.seq;
    snprintf(s_req.verb, sizeof(s_req.verb), "%s", c.verb);
    snprintf(s_req.args, sizeof(s_req.args), "%s", c.args);
    s_req_state.store(ReqState::Pending, std::memory_order_release);
    // The UI task polls every ~32 ms; a page's consoleCommand may itself
    // start work (a directory listing) but replies before it completes.
    for (int waited = 0; waited < 3000; waited += 5) {
        if (s_req_state.load(std::memory_order_acquire) == ReqState::Done) {
            printf("%s\n", s_reply);
            s_req_state.store(ReqState::Idle, std::memory_order_release);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    // Give up on the reply but leave the slot to the UI task: it will still
    // write s_reply and flip to Done, at which point the next request may
    // proceed. Reporting a timeout is the honest answer here.
    reply_err(c.seq, "timeout");
}

void dispatch(const Command& c) {
    const int32_t seq = c.seq;
    const char* p = c.args;

    if (c.legacy) {
        // Seq-less lines from the human-driven scripts keep their legacy
        // replies. Unknown legacy verbs are ignored, as they always were.
        if (!strcmp(c.verb, "LOG")) {
            handle_log_command(c.args);
        } else if (!strcmp(c.verb, "SCREENSHOT")) {
            wavex_screenshot_request();
        }
        return;
    }
    if (c.verb[0] == '\0') {
        reply_err(seq, "syntax");
        return;
    }

    if (!strcmp(c.verb, "PING")) {
        reply_ok(seq);
    } else if (!strcmp(c.verb, "LOG")) {
        if (handle_log_command(c.args)) {
            reply_ok(seq);
        } else {
            reply_err(seq, "badlog");
        }
    } else if (!strcmp(c.verb, "SCREENSHOT")) {
        if (wavex_screenshot_request()) {
            reply_ok(seq);
        } else {
            reply_err(seq, "busy");
        }
    } else if (!strcmp(c.verb, "KEY")) {
        // KEY <name> [PRESS|RELEASE|TAP]: any panel key by its panelKeyName()
        // (SOFT3, SAMPLE, TRACK_NEXT, PAD16 ...), plus the old SELECT/ENC
        // spellings. Posted exactly as the keypad task posts a matrix key, so
        // the dispatcher's key semantics are what gets exercised.
        char name[16], action[16];
        NextWord(&p, name, sizeof(name));
        if (!NextWord(&p, action, sizeof(action))) {
            snprintf(action, sizeof(action), "TAP");
        }
        const wavex_ui::PanelKey key = wavex_ui::panelKeyFromName(name);
        if (key == wavex_ui::PanelKey::None) {
            reply_err(seq, "badkey");
            return;
        }
        const auto id = static_cast<uint8_t>(key);
        bool ok = true;
        if (!strcmp(action, "PRESS") || !strcmp(action, "TAP")) {
            ok = post_input(wavex_ui::InputType::KeyPress, id, 0) && ok;
        }
        if (!strcmp(action, "RELEASE") || !strcmp(action, "TAP")) {
            ok = post_input(wavex_ui::InputType::KeyRelease, id, 0) && ok;
        }
        if (strcmp(action, "PRESS") && strcmp(action, "RELEASE") && strcmp(action, "TAP")) {
            reply_err(seq, "badaction");
            return;
        }
        ok ? reply_ok(seq) : reply_err(seq, "queuefull");
    } else if (!strcmp(c.verb, "ENC") || !strcmp(c.verb, "POT")) {
        // ENC <delta> / POT <delta>: one event carrying the magnitude, the
        // direction in the type - exactly what the UI task's poll posts.
        long delta = 0;
        if (!NextInt(&p, &delta) || delta == 0) {
            reply_err(seq, "baddelta");
            return;
        }
        const bool enc = !strcmp(c.verb, "ENC");
        const wavex_ui::InputType type =
            delta > 0 ? (enc ? wavex_ui::InputType::EncoderRight : wavex_ui::InputType::EncoderUp)
                      : (enc ? wavex_ui::InputType::EncoderLeft : wavex_ui::InputType::EncoderDown);
        const int16_t mag = static_cast<int16_t>(delta > 0 ? delta : -delta);
        post_input(type, 0, mag) ? reply_ok(seq) : reply_err(seq, "queuefull");
    } else if (!strcmp(c.verb, "TAP")) {
        // TAP <x> <y>: press-hold-release through the synthetic indev.
        long x, y;
        if (!NextInt(&p, &x) || !NextInt(&p, &y)) {
            reply_err(seq, "badxy");
            return;
        }
        bool ok = true;
        for (int i = 0; i < kTapHoldReads; ++i) {
            ok = touch_enqueue(static_cast<int16_t>(x), static_cast<int16_t>(y), true) && ok;
        }
        ok = touch_enqueue(static_cast<int16_t>(x), static_cast<int16_t>(y), false) && ok;
        ok ? reply_ok(seq) : reply_err(seq, "queuefull");
    } else if (!strcmp(c.verb, "TOUCH")) {
        // TOUCH <DOWN|MOVE|UP> <x> <y>: individual pointer transitions.
        char what[8];
        long x, y;
        NextWord(&p, what, sizeof(what));
        if (!NextInt(&p, &x) || !NextInt(&p, &y)) {
            reply_err(seq, "badxy");
            return;
        }
        bool pressed;
        if (!strcmp(what, "DOWN") || !strcmp(what, "MOVE")) {
            pressed = true;
        } else if (!strcmp(what, "UP")) {
            pressed = false;
        } else {
            reply_err(seq, "badtouch");
            return;
        }
        touch_enqueue(static_cast<int16_t>(x), static_cast<int16_t>(y), pressed)
            ? reply_ok(seq)
            : reply_err(seq, "queuefull");
    } else if (!strcmp(c.verb, "STATE") || !strcmp(c.verb, "PAGE") || !strcmp(c.verb, "TRACK") ||
               !strcmp(c.verb, "HOME")) {
        run_on_ui_task(c);
    } else {
        reply_err(seq, "unknown");
    }
}

void console_task(void*) {
    uint8_t buf[64];
    LineReader reader;
    for (;;) {
        const int n = uart_read_bytes(kUart, buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            if (reader.Feed(static_cast<char>(buf[i]))) {
                Command c;
                if (ParseCommand(reader.Line(), c)) {
                    dispatch(c);
                }
                reader.Release();
            }
        }
        wavex_screenshot_service();
    }
}

// ---------------------------------------------------------------------------
// UI-task verbs (wavex_console_poll, under the LVGL lock)
// ---------------------------------------------------------------------------

const char* binding_state_name(uint8_t state) {
    switch (state) {
        case WaveX::Protocol::TRACK_BINDING_EMPTY:
            return "empty";
        case WaveX::Protocol::TRACK_BINDING_SAMPLE:
            return "sample";
        case WaveX::Protocol::TRACK_BINDING_PATCH:
            return "instrument";
        case WaveX::Protocol::TRACK_BINDING_LOADING:
            return "loading";
        default:
            return "?";
    }
}

size_t append_track_state(char* out, size_t cap, size_t len, uint8_t track) {
    len = AppendKvInt(out, cap, len, "track", track);
    WaveX::Protocol::TrackBindingMessage b;
    if (inter_mcu_get_track_binding(track, &b)) {
        len = AppendKv(out, cap, len, "tstate", binding_state_name(b.state));
        len = AppendKvInt(out, cap, len, "tid", b.sample_id);
        len = AppendKvText(out, cap, len, "tname", b.name);
    } else {
        len = AppendKv(out, cap, len, "tstate", "unknown");
    }
    return len;
}

void serve_state(int32_t seq) {
    auto& nav = wavex_ui::UINavigator::instance();
    auto page = nav.active();
    size_t len = FormatOk(seq, s_reply, sizeof(s_reply));
    len = AppendKvText(s_reply, sizeof(s_reply), len, "page", page ? page->name() : "-");
    len = AppendKvInt(s_reply, sizeof(s_reply), len, "depth", static_cast<long>(nav.depth()));
    len = AppendKvInt(s_reply, sizeof(s_reply), len, "shift", nav.isShifted() ? 1 : 0);
    len = AppendKv(
        s_reply, sizeof(s_reply), len, "root", wavex_ui::rootGroupName(nav.activeRootGroup()));
    len = AppendKv(s_reply,
                   sizeof(s_reply),
                   len,
                   "lastkey",
                   wavex_ui::panelKeyName(wavex_ui::InputDispatcher::instance().lastKey()));
    len = append_track_state(s_reply, sizeof(s_reply), len, wavex_ui::getCurrentTrack());
    // The softkey row as the bar shows it now (shifted or not), with each
    // button's centre so a host can TAP it through the real touch path.
    if (auto* bar = nav.softkeyBar()) {
        for (int i = 0; i < wavex_ui::NUM_SOFTKEYS; ++i) {
            const auto& k = bar->key(i);
            char key[8];
            snprintf(key, sizeof(key), "sk%d", i);
            len = AppendKvText(
                s_reply, sizeof(s_reply), len, key, k.label.empty() ? "-" : k.label.c_str());
            if (!k.label.empty()) {
                char en[10];
                snprintf(en, sizeof(en), "sk%den", i);
                len = AppendKvInt(s_reply, sizeof(s_reply), len, en, k.enabled ? 1 : 0);
                int32_t x, y;
                if (bar->buttonCenter(i, &x, &y)) {
                    char xy[10], v[24];
                    snprintf(xy, sizeof(xy), "sk%dxy", i);
                    snprintf(v, sizeof(v), "%ld,%ld", static_cast<long>(x), static_cast<long>(y));
                    len = AppendKv(s_reply, sizeof(s_reply), len, xy, v);
                }
            }
        }
    }
    len = AppendKvInt(s_reply,
                      sizeof(s_reply),
                      len,
                      "dropped",
                      static_cast<long>(wavex_ui::InputDispatcher::instance().droppedEvents()));
    if (page) {
        len = page->consoleState(s_reply, sizeof(s_reply), len);
    }
    (void)len;
}

void serve_request() {
    const int32_t seq = s_req.seq;
    if (!strcmp(s_req.verb, "STATE")) {
        serve_state(seq);
    } else if (!strcmp(s_req.verb, "TRACK")) {
        // TRACK <n>: select a Track (0-based, as on the wire) and ask the
        // Daisy for its binding, the way the Track -/+ keys do.
        const char* p = s_req.args;
        long n;
        if (!NextInt(&p, &n) || n < 0 || n >= WAVEX_MIX_TRACKS) {
            FormatErr(seq, "badtrack", s_reply, sizeof(s_reply));
        } else {
            wavex_ui::setCurrentTrack(static_cast<uint8_t>(n));
            const esp_err_t req = inter_mcu_request_track_binding(static_cast<uint8_t>(n));
            wavex_ui::UINavigator::instance().refreshSoftkeys();
            size_t len = FormatOk(seq, s_reply, sizeof(s_reply));
            len = AppendKv(s_reply, sizeof(s_reply), len, "req", esp_err_to_name(req));
            append_track_state(s_reply, sizeof(s_reply), len, static_cast<uint8_t>(n));
        }
    } else if (!strcmp(s_req.verb, "HOME")) {
        wavex_ui::UINavigator::instance().popToRoot();
        FormatOk(seq, s_reply, sizeof(s_reply));
    } else if (!strcmp(s_req.verb, "PAGE")) {
        auto page = wavex_ui::UINavigator::instance().active();
        char body[256] = {};
        if (page && page->consoleCommand(s_req.args, body, sizeof(body))) {
            size_t len = FormatOk(seq, s_reply, sizeof(s_reply));
            if (body[0]) {
                snprintf(s_reply + len, sizeof(s_reply) - len, " %s", body);
            }
        } else {
            FormatErr(seq, body[0] ? body : "unknown", s_reply, sizeof(s_reply));
        }
    } else {
        FormatErr(seq, "unknown", s_reply, sizeof(s_reply));
    }
}

}  // namespace

void wavex_console_start() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;

    // RX-only driver on the console UART. Console TX (logging, printf) does
    // not go through this driver, so output is unaffected.
    if (!uart_is_driver_installed(kUart)) {
        const esp_err_t err = uart_driver_install(kUart, 1024, 0, 0, nullptr, 0);
        if (err != ESP_OK) {
            ESP_LOGW(
                TAG, "uart_driver_install failed (%s) - console disabled", esp_err_to_name(err));
            return;
        }
    }

    s_touch_q = xQueueCreate(32, sizeof(TouchStep));
    lvgl_port_lock(portMAX_DELAY);
    s_touch_indev = lv_indev_create();
    if (s_touch_indev) {
        lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_touch_indev, touch_read_cb);
        lv_indev_set_display(s_touch_indev, lv_display_get_default());
    }
    lvgl_port_unlock();

    xTaskCreate(console_task, "console", 6144, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "Debug console armed (WAVEX-DBG <seq> <VERB>; legacy WAVEX-LOG/SCREENSHOT)");
}

void wavex_console_poll() {
    if (s_req_state.load(std::memory_order_acquire) != ReqState::Pending) {
        return;
    }
    lvgl_port_lock(portMAX_DELAY);
    serve_request();
    lvgl_port_unlock();
    s_req_state.store(ReqState::Done, std::memory_order_release);
}

#endif  // WAVEX_DEBUG_HARNESS_ENABLED
