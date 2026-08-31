// WaveX UI Settings Page
#pragma once

#include <lvgl.h>

#include "input_event.h"
#include "ui_navigator.h"
#include "ui_page.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wavex_ui {

/**
 * @brief What a settings row is, which decides how it draws and whether the
 *        encoder can land on it.
 *
 * `Unimplemented` exists because the alternative - a row that looks editable,
 * moves when you turn the encoder, and quietly logs the new value to a serial
 * port nobody is watching - is worse than no row at all. Anything the firmware
 * cannot actually do says so on the panel.
 */
enum class SettingKind {
    Value,          ///< Editable integer, adjusted with the encoder
    Info,           ///< Read-only text in the value column (version, size, ...)
    Unimplemented,  ///< Read-only, dimmed, value column says it does nothing
};

struct Setting {
    std::string label;
    int value = 0;
    int minValue = 0;
    int maxValue = 0;
    std::function<void(int)> onChange;

    SettingKind kind = SettingKind::Value;

    /// Info/Unimplemented: the text drawn in the value column. For
    /// Unimplemented it is the reason, not a value.
    std::string text;

    /// Value rows only: renders the number as something a person reads
    /// ("Omni", "C#3") instead of the raw integer. Null means "%d".
    std::function<std::string(int)> format;

    bool editable() const { return kind == SettingKind::Value; }
};

/**
 * @brief Settings page: a scrolling list of rows, encoder-edited.
 *
 * Rows are `Setting`s. Editable ones adjust with the encoder while in edit
 * mode; `Info` and `Unimplemented` rows are skipped by the selection entirely,
 * so the encoder cannot come to rest on something it cannot change.
 *
 * Layout is against the navigator's content area (1280 wide, 545 high, less a
 * 56 px tab bar when hosted in a tab group), not a fixed 480x320 box - the
 * panel has not been that size for a long time and the old list clipped its
 * own contents on any page with more than six rows.
 */
class UISettingsPage : public UIPage {
   public:
    explicit UISettingsPage(std::string title) : title_(std::move(title)) {}

    const char* name() const override { return title_.c_str(); }

    /// onChange fires on the UI task when the encoder commits a new value.
    void addSetting(const std::string& label,
                    int value,
                    int minValue,
                    int maxValue,
                    std::function<void(int)> onChange,
                    std::function<std::string(int)> format = nullptr) {
        Setting s;
        s.label = label;
        s.value = value;
        s.minValue = minValue;
        s.maxValue = maxValue;
        s.onChange = std::move(onChange);
        s.kind = SettingKind::Value;
        s.format = std::move(format);
        settings_.push_back(std::move(s));
    }

    /// A read-only row stating a fact about the system, not a control.
    void addInfo(const std::string& label, const std::string& text) {
        Setting s;
        s.label = label;
        s.kind = SettingKind::Info;
        s.text = text;
        settings_.push_back(std::move(s));
    }

    /**
     * @brief Add a row that names something the firmware does not do yet.
     * @param reason Short, plain wording shown in the value column. It is read
     *               on the panel by someone deciding whether the control is
     *               broken, so say what is missing, not "TODO".
     */
    void addUnimplemented(const std::string& label, const std::string& reason) {
        Setting s;
        s.label = label;
        s.kind = SettingKind::Unimplemented;
        s.text = reason;
        settings_.push_back(std::move(s));
    }

    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;

   protected:
    // Protected (not private) so purpose-built settings pages - e.g. the CV
    // calibration page - can subclass, reuse the list/edit mechanics, and
    // provide their own softkeys and value refresh.
    std::string title_;
    std::vector<Setting> settings_;
    int selectedSetting_ = -1;  ///< -1 when the page has no editable row
    bool editingValue_ = false;

    lv_obj_t* list_ = nullptr;
    std::vector<lv_obj_t*> rows_;
    std::vector<lv_obj_t*> valueLabels_;

    /// Row currently carrying the selected/editing overlay style. Tracked so a
    /// selection change restyles two rows instead of invalidating every one.
    int styledRow_ = -1;

    /// True when at least one row can be edited.
    bool hasEditableRow() const;

    /**
     * @brief Rebuild the settings list display.
     *
     * Full teardown and rebuild - page-entry cost, so it uses shared styles
     * rather than per-object style properties. Selection changes go through
     * refreshSelection() instead, which touches only the two affected rows.
     */
    void rebuildList();

    /// Restyles rows to match selectedSetting_/editingValue_.
    void refreshSelection();

    /// +1 down, -1 up; skips rows that cannot be edited.
    void moveSelection(int delta);

    void adjustValue(int delta);
    void toggleEditMode();
    void updateSetting(int settingIndex, int newValue);

    /// Renders one row's value column into `out`.
    void formatValue(const Setting& s, char* out, size_t out_len) const;
};

}  // namespace wavex_ui
