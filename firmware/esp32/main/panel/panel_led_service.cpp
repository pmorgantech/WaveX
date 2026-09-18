#include "ui/panel/panel_led_service.h"

#include "esp_timer.h"
#include "inter_mcu.h"
#include "panel_task.h"
#include "ui/display_manager.h"
#include "ui/ui_navigator.h"
#include "ui/ui_softkey_bar.h"
namespace wavex_ui {
namespace {
int test_channel = -1;
uint32_t test_started = 0;
uint32_t nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}
}  // namespace
void PanelLedWalk() {
    test_channel = test_channel < 0 ? 0 : (test_channel + 1) % WAVEX_LED_CHANNELS;
    test_started = nowMs();
}
void PanelLedAll() {
    test_channel = test_channel == -2 ? -1 : -2;
    test_started = nowMs();
}
void PanelLedTestOff() {
    test_channel = -1;
}
int PanelLedTestChannel() {
    return test_channel;
}
void ServicePanelLeds() {
    const uint32_t now = nowMs();
    if (now - test_started >= 10000)
        PanelLedTestOff();
    auto& nav = UINavigator::instance();
    PanelLedInputs input;
    input.root = nav.activeRootGroup();
    input.shifted = nav.isShifted();
    input.blanked = DisplayManager::instance().screenBlanked();
    WaveX::Protocol::SeqPlayheadMessage head;
    input.playing =
        inter_mcu_backend_link_alive() && inter_mcu_get_seq_playhead(&head) && head.playing == 1;
    if (auto* bar = nav.softkeyBar()) {
        for (int i = 0; i < NUM_SOFTKEYS; ++i) {
            const auto& key = bar->key(i);
            input.soft_defined[i] = !key.label.empty();
            input.soft_enabled[i] = key.enabled;
            input.soft_active[i] = key.active;
        }
    }
    if (auto page = nav.active())
        input.pads = page->panelLeds();
    auto frame = BuildPanelLedFrame(input);
    frame.test_channel = static_cast<int16_t>(test_channel >= 0 ? test_channel : -1);
    frame.test_all = test_channel == -2;
    wavex_panel::Publish(frame, now);
}
}  // namespace wavex_ui
