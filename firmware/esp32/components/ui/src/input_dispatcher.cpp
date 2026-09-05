#include "ui/input_dispatcher.h"

#include "config/hardware_config.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/display_manager.h"
#include "ui/ui_navigator.h"
#include "ui/ui_softkey.h"

static const char* TAG = "INPUT";

namespace wavex_ui {

static constexpr size_t kQueueLength = 64;

InputDispatcher& InputDispatcher::instance() {
    static InputDispatcher inst;
    return inst;
}

InputDispatcher::InputDispatcher() {
    queue_ = xQueueCreate(kQueueLength, sizeof(InputEvent));
}

bool InputDispatcher::postFromISR(const InputEvent& evt, BaseType_t* hpTaskWoken) {
    if (!queue_)
        return false;
    const bool posted = xQueueSendFromISR(queue_, &evt, hpTaskWoken) == pdTRUE;
    if (!posted) {
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
    }
    return posted;
}

bool InputDispatcher::post(const InputEvent& evt, TickType_t ticksToWait) {
    if (!queue_)
        return false;
    // Callers mostly ignore the return value, and a dropped event is a
    // keypress or detent the instrument never saw. Counting it here means the
    // loss is visible on the diagnostics page instead of being silent.
    const bool posted = xQueueSend(queue_, &evt, ticksToWait) == pdTRUE;
    if (!posted) {
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
    }
    return posted;
}

void InputDispatcher::processAll() {
    if (!queue_)
        return;
    InputEvent evt;
    while (xQueueReceive(queue_, &evt, 0) == pdTRUE) {
        // Button and encoder producers converge here. Unlike a page-local
        // handler this catches every physical button and encoder movement,
        // even when no page consumes the event.
        DisplayManager::instance().noteUserActivity();

        // Handlers build and restyle widgets, so dispatch runs under the LVGL
        // port lock; the LVGL task renders on the other core and an unlocked
        // handler corrupts the object tree.
        //
        // Taken per event rather than once around the whole drain: a backlog
        // (a fast encoder spin queued while a page was still building) would
        // otherwise hold the lock for every event in it back to back, stalling
        // the render task for as many frames as there are events. Per event the
        // hold is one handler long and the renderer interleaves. The extra
        // acquire/release is a few hundred cycles against a queue that carries
        // single-digit events per 32 ms pass.
        //
        // Lock order is LVGL -> UART: handlers send over the link
        // (inter_mcu_send_*), which takes s_uart_mutex briefly and with a
        // timeout. Nothing may take these in the other order - in particular
        // the UART RX task's callbacks must stay flag-only, never touching
        // LVGL, or this becomes a deadlock.
        lvgl_port_lock(portMAX_DELAY);
        dispatch(evt);
        lvgl_port_unlock();
    }
}

void InputDispatcher::dispatch(InputEvent evt) {
    const bool is_key = evt.type == InputType::KeyPress || evt.type == InputType::KeyRelease ||
                        evt.type == InputType::ButtonPress || evt.type == InputType::ButtonRelease;
    if (!is_key) {
        if (current_) {
            current_->handleEvent(evt);
        }
        return;
    }

    // The key semantics of panel-controls.md §4.3 live here and nowhere else.
    // The global keys are never forwarded: no page distinguishes key ids, so
    // forwarded they would read as "activate" - which is what the physical
    // Back key did before it was intercepted (found by the HIL suite,
    // 2026-09-04). Pages see modifier state, not the Shift key; they see the
    // Track change through onTrackChanged(), not the key.
    const PanelKey key = evt.key();
    const bool press = evt.isKeyPress();
    if (press) {
        last_key_.store(key, std::memory_order_relaxed);
        key_presses_.fetch_add(1, std::memory_order_relaxed);
    }
    auto& nav = UINavigator::instance();

    switch (key) {
        case PanelKey::Shift:
            if (press) {
                nav.toggleShift();
            }
            return;
        case PanelKey::Back:
            // The root page stays put (pop() refuses).
            if (press) {
                nav.pop();
            }
            return;
        case PanelKey::Soft1:
        case PanelKey::Soft2:
        case PanelKey::Soft3:
        case PanelKey::Soft4:
        case PanelKey::Soft5:
        case PanelKey::Soft6:
            // Whatever the bar shows now - the shifted row while Shift is
            // latched - exactly as a touch on that button would.
            if (press) {
                nav.softkeyBar()->press(softkeyIndex(key));
            }
            return;
        case PanelKey::JumpSample:
            if (press) {
                nav.jumpToRoot(RootGroup::Sample);
            }
            return;
        case PanelKey::JumpPlay:
            if (press) {
                nav.jumpToRoot(RootGroup::Play);
            }
            return;
        case PanelKey::JumpInstrument:
            if (press) {
                nav.jumpToRoot(RootGroup::Instrument);
            }
            return;
        case PanelKey::JumpTrack:
            if (press) {
                nav.jumpToRoot(RootGroup::Track);
            }
            return;
        case PanelKey::JumpMixer:
            if (press) {
                nav.jumpToRoot(RootGroup::Mixer);
            }
            return;
        case PanelKey::JumpSettings:
            if (press) {
                nav.jumpToRoot(RootGroup::Settings);
            }
            return;
        case PanelKey::TrackPrev:
            if (press) {
                stepTrack(-1);
            }
            return;
        case PanelKey::TrackNext:
            if (press) {
                stepTrack(+1);
            }
            return;
        case PanelKey::NavAPush:
        case PanelKey::NavBPush:
            // Select and the encoder push are page-handled, and every page
            // reads them as the ButtonPress it has always received.
            evt.type = press ? InputType::ButtonPress : InputType::ButtonRelease;
            evt.source_id = key == PanelKey::NavAPush ? BUTTON_SELECT : BUTTON_ENCODER_CLICK;
            break;
        default:
            // Transport, pads and anything unmapped go to the page as posted
            // until Phase 2 gives them sequencer semantics. The default
            // onInput ignores them.
            break;
    }
    if (current_) {
        current_->handleEvent(evt);
    }
}

void InputDispatcher::stepTrack(int delta) {
    // Wraps: 16 Tracks matches instrument.hpp's kNumTracks and MSG_NOTE_ON's
    // channel & 0x0F on the backend.
    constexpr int kTracks = WAVEX_MIX_TRACKS;
    const auto next = static_cast<uint8_t>((getCurrentTrack() + delta + kTracks) % kTracks);
    setCurrentTrack(next);
    // Ask the Daisy what the Track holds; the reply lands on whichever page
    // shows a binding status, through its own deferred-update path.
    inter_mcu_request_track_binding(next);
    auto& nav = UINavigator::instance();
    if (auto page = nav.active()) {
        page->onTrackChanged();
    }
    nav.refreshSoftkeys();
    ESP_LOGI(TAG, "Track %u", trackDisplayNumber(next));
}

void InputDispatcher::setActiveContext(std::shared_ptr<UIContext> ctx) {
    current_ = std::move(ctx);
}

}  // namespace wavex_ui
