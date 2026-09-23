#include "config/hardware_config.h"
#include "debug/console_command.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_settings_page.h"
#include "usb_midi_port.h"

namespace wavex_ui {
namespace {
using wavex_midi::UsbMode;
const char* modeName(UsbMode mode) {
    return mode == UsbMode::Host ? "Host" : "Device";
}
class MidiSettingsPage : public UISettingsPage {
   public:
    MidiSettingsPage() : UISettingsPage("MIDI") {}
    void onEnter(lv_obj_t* parent) override {
        settings_.clear();
        const auto state = wavex_midi::ReadUsbPort();
        draft_ = state.saved;
#if WAVEX_ESP_USB_MIDI_ENABLED && (WAVEX_USB_MIDI_INPUT_ENABLED || WAVEX_USB_MIDI_OUTPUT_ENABLED)
        addSetting(
            "USB port mode",
            static_cast<int>(draft_),
            0,
            1,
            [this](int value) {
                draft_ = static_cast<UsbMode>(value);
                refresh();
            },
            [](int value) { return std::string(modeName(static_cast<UsbMode>(value))); });
#else
        addInfo("USB port mode", "disabled in build");
#endif
        addInfo("USB active mode", "");
        addInfo("USB connection", "");
        addInfo("USB saved mode", "");
        addInfo("USB mode change", "");
        addInfo("USB Device", "Connect to computer / DAW");
        addInfo("USB Host", "MIDI 1.0 adapter / first cable");
        addInfo("Receive channel", "per Track - see Project");
        addInfo("DIN MIDI in", WAVEX_ESP_DIN_MIDI_ENABLED ? "enabled" : "disabled in build");
        addUnimplemented("Velocity curve", "unchanged / not implemented");
        addInfo("Clock source", "Internal or MIDI - Sequencer");
        addInfo("MIDI clock out", "DIN / USB - Sequencer");
        addInfo("Expression", "CC1 / Pressure - Instrument Mod");
        addInfo("Controller reset", "CC121 clears expression");
        addUnimplemented("Track routing save", "routing resets at boot");
        UISettingsPage::onEnter(parent);
        refresh();
        timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<MidiSettingsPage*>(lv_timer_get_user_data(timer))->refresh();
            },
            100,
            this);
    }
    void onExit() override {
        if (timer_)
            lv_timer_delete(timer_);
        timer_ = nullptr;
        UISettingsPage::onExit();
    }
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override {
        auto keys = UISettingsPage::getSoftkeys();
        const auto state = wavex_midi::ReadUsbPort();
        keys[2] = {"Device / Host",
                   [this] { updateSetting(0, draft_ == UsbMode::Device ? 1 : 0); },
                   state.connection != wavex_midi::UsbConnection::Disabled && !state.saving,
                   "USB disabled or saving"};
        keys[4] = {"Save mode",
                   [this] {
                       wavex_midi::SaveUsbMode(draft_);
                       refresh();
                   },
                   state.connection != wavex_midi::UsbConnection::Disabled && !state.saving &&
                       (draft_ != state.saved || state.save_result != ESP_OK),
                   "No change or save in progress"};
        return keys;
    }
    size_t consoleState(char* out, size_t cap, size_t len) override {
        const auto state = wavex_midi::ReadUsbPort();
        using WaveX::Debug::AppendKvInt;
        len = AppendKvInt(out, cap, len, "usbmode", static_cast<int>(state.active));
        len = AppendKvInt(out, cap, len, "usbsaved", static_cast<int>(state.saved));
        len = AppendKvInt(out, cap, len, "usbconnection", static_cast<int>(state.connection));
        len = AppendKvInt(out, cap, len, "usbsaving", state.saving);
        return AppendKvInt(out, cap, len, "usbsaveerror", state.save_result);
    }

   private:
    void info(size_t row, const char* value) {
        if (settings_[row].text == value)
            return;
        settings_[row].text = value;
        if (row < valueLabels_.size())
            lv_label_set_text(valueLabels_[row], value);
    }
    void refresh() {
        const auto state = wavex_midi::ReadUsbPort();
        info(1, modeName(state.active));
        using Connection = wavex_midi::UsbConnection;
        const char* connection = "Disabled";
        switch (state.connection) {
            case Connection::Starting:
                connection = "Starting";
                break;
            case Connection::Waiting:
                connection = "Waiting for connection";
                break;
            case Connection::Connected:
                connection = "Connected";
                break;
            case Connection::Unsupported:
                connection = "Unsupported adapter";
                break;
            case Connection::Error:
                connection = "Error - reconnect / restart";
                break;
            case Connection::Disabled:
                break;
        }
        info(2, connection);
        info(3, modeName(state.saved));
        info(4,
             state.saving                  ? "Saving..."
             : state.save_result != ESP_OK ? "Save failed - retry"
             : draft_ != state.saved       ? "Select, then Save mode"
             : state.saved != state.active ? "Saved - restart to apply"
                                           : "Save + restart to change");
        const unsigned flags = unsigned(state.saving) | (unsigned(draft_ != state.saved) << 1) |
                               (unsigned(state.save_result != ESP_OK) << 2) |
                               (unsigned(state.connection == Connection::Disabled) << 3);
        if (flags != soft_state_) {
            soft_state_ = flags;
            UINavigator::instance().refreshSoftkeys();
        }
    }
    UsbMode draft_ = UsbMode::Device;
    lv_timer_t* timer_ = nullptr;
    unsigned soft_state_ = ~0u;
};
}  // namespace
std::shared_ptr<UIPage> createMidiSettingsPage() {
    return std::make_shared<MidiSettingsPage>();
}
}  // namespace wavex_ui
