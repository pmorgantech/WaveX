// WaveX UI Settings Page Implementation
#include "ui/ui_settings_page.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "esp_lvgl_port.h"
#include "ui/ui_palette.h"

#include <cstdio>

// LVGL locking macros. The port mutex is recursive (esp_lvgl_port uses
// xSemaphoreTakeRecursive), so taking it here is safe even though onEnter is
// called with it already held by the navigator.
#define LV_LOCK() lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

static const char* TAG = "UI_SETTINGS_PAGE";

namespace wavex_ui {

using namespace wavex_ui::palette;

namespace {

// Geometry. The navigator's content area is the panel (1280x720 rotated) less
// the 75 px header and the 100 px softkey row; a tab group takes 56 more off
// the top of that. Rows are sized so a full page of them scrolls rather than
// clipping, which the old fixed 460x250 list did on anything past six rows.
constexpr int kRowH = 46;
constexpr int kRowGap = 4;
constexpr int kListPad = 10;
constexpr int kValueLabelW = 320;

// Shared styles: one set for the whole component, referenced by every row.
//
// Every lv_obj_set_style_*() call stores a property in the object's own style
// list, which allocates. A settings page builds up to a dozen rows of three
// objects each, and page entry is what this UI pays for (docs/backlog.md), so
// the properties that are identical across rows live in one style each and the
// rows only add/remove them.
//
// Function-local statics: initialised once, never destroyed, which is what an
// lv_style_t referenced by live objects requires. UI task only.
struct RowStyles {
    lv_style_t list;
    lv_style_t row;
    lv_style_t row_selected;
    lv_style_t row_editing;
    lv_style_t name;
    lv_style_t name_dim;
    lv_style_t value;
    lv_style_t value_dim;
};

const RowStyles& rowStyles() {
    static RowStyles s;
    static bool ready = false;
    if (ready) {
        return s;
    }

    lv_style_init(&s.list);
    lv_style_set_bg_opa(&s.list, LV_OPA_COVER);
    lv_style_set_bg_color(&s.list, lv_color_hex(kColBg));
    lv_style_set_border_width(&s.list, 0);
    lv_style_set_radius(&s.list, 0);
    lv_style_set_pad_all(&s.list, kListPad);
    lv_style_set_pad_row(&s.list, kRowGap);
    lv_style_set_layout(&s.list, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s.list, LV_FLEX_FLOW_COLUMN);

    lv_style_init(&s.row);
    lv_style_set_bg_opa(&s.row, LV_OPA_COVER);
    lv_style_set_bg_color(&s.row, lv_color_hex(kColCard));
    lv_style_set_border_width(&s.row, 1);
    lv_style_set_border_color(&s.row, lv_color_hex(kColBorder));
    lv_style_set_radius(&s.row, 4);
    lv_style_set_pad_left(&s.row, 16);
    lv_style_set_pad_right(&s.row, 16);

    // Selected / editing are additive overlays on top of `row`, so a selection
    // change is two add_style/remove_style calls rather than a rebuild.
    lv_style_init(&s.row_selected);
    lv_style_set_bg_color(&s.row_selected, lv_color_hex(kColTabOn));
    lv_style_set_border_color(&s.row_selected, lv_color_hex(kColBlue));
    lv_style_set_border_width(&s.row_selected, 2);

    lv_style_init(&s.row_editing);
    lv_style_set_bg_color(&s.row_editing, lv_color_hex(kColTabOn));
    lv_style_set_border_color(&s.row_editing, lv_color_hex(kColOrange));
    lv_style_set_border_width(&s.row_editing, 2);

    lv_style_init(&s.name);
    lv_style_set_text_font(&s.name, &lv_font_montserrat_18);
    lv_style_set_text_color(&s.name, lv_color_white());

    lv_style_init(&s.name_dim);
    lv_style_set_text_font(&s.name_dim, &lv_font_montserrat_18);
    lv_style_set_text_color(&s.name_dim, lv_color_hex(kColDim));

    lv_style_init(&s.value);
    lv_style_set_text_font(&s.value, &lv_font_montserrat_18);
    lv_style_set_text_color(&s.value, lv_color_hex(kColGreen));
    lv_style_set_text_align(&s.value, LV_TEXT_ALIGN_RIGHT);
    lv_style_set_width(&s.value, kValueLabelW);

    lv_style_init(&s.value_dim);
    lv_style_set_text_font(&s.value_dim, &lv_font_montserrat_18);
    lv_style_set_text_color(&s.value_dim, lv_color_hex(kColDimmer));
    lv_style_set_text_align(&s.value_dim, LV_TEXT_ALIGN_RIGHT);
    lv_style_set_width(&s.value_dim, kValueLabelW);

    ready = true;
    return s;
}

}  // namespace

void UISettingsPage::formatValue(const Setting& s, char* out, size_t out_len) const {
    if (s.kind != SettingKind::Value) {
        snprintf(out, out_len, "%s", s.text.c_str());
        return;
    }
    if (s.format) {
        snprintf(out, out_len, "%s", s.format(s.value).c_str());
        return;
    }
    snprintf(out, out_len, "%d", s.value);
}

bool UISettingsPage::hasEditableRow() const {
    for (const auto& s: settings_) {
        if (s.editable()) {
            return true;
        }
    }
    return false;
}

void UISettingsPage::onEnter(lv_obj_t* parent) {
    // NOTE: onEnter is called from UINavigator::push/pop (or UITabHostPage)
    // which already holds the LVGL lock.
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kColBg), LV_PART_MAIN);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // No title label: the navigator header names the page and, inside a tab
    // group, the tab bar names it again. A third copy only costs rows.
    list_ = lv_obj_create(root_);
    lv_obj_remove_style_all(list_);
    lv_obj_add_style(list_, &rowStyles().list, LV_PART_MAIN);
    lv_obj_set_size(list_, lv_pct(100), lv_pct(100));
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_AUTO);

    // Land the selection on the first row that can actually be edited.
    if (selectedSetting_ < 0 || selectedSetting_ >= static_cast<int>(settings_.size()) ||
        !settings_[static_cast<size_t>(selectedSetting_)].editable()) {
        selectedSetting_ = -1;
        for (size_t i = 0; i < settings_.size(); ++i) {
            if (settings_[i].editable()) {
                selectedSetting_ = static_cast<int>(i);
                break;
            }
        }
    }
    editingValue_ = false;

    rebuildList();
}

void UISettingsPage::onExit() {
    // NOTE: onExit is called with the LVGL lock already held (see onEnter).
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        list_ = nullptr;
        rows_.clear();
        valueLabels_.clear();
        styledRow_ = -1;
    }
    editingValue_ = false;
}

void UISettingsPage::onInput(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::EncoderLeft:
            if (editingValue_) {
                adjustValue(-1);
            } else {
                moveSelection(-1);
            }
            break;
        case InputType::EncoderRight:
            if (editingValue_) {
                adjustValue(+1);
            } else {
                moveSelection(+1);
            }
            break;
        case InputType::ButtonPress:
        case InputType::EncoderClick:
            toggleEditMode();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UISettingsPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};

    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};

    // A disabled key with a reason beats a key that toggles a mode nothing
    // responds to - read-only pages (System, Storage) have nothing to edit.
    if (hasEditableRow()) {
        keys[1] = {editingValue_ ? "Done" : "Edit", [this]() { toggleEditMode(); }};
    } else {
        keys[1] = {"Edit", nullptr, false, "nothing on this page is editable"};
    }

    return keys;
}

void UISettingsPage::rebuildList() {
    if (!list_)
        return;
    LV_LOCK();

    lv_obj_clean(list_);
    rows_.clear();
    valueLabels_.clear();
    styledRow_ = -1;
    rows_.reserve(settings_.size());
    valueLabels_.reserve(settings_.size());

    const RowStyles& st = rowStyles();

    for (size_t i = 0; i < settings_.size(); ++i) {
        const Setting& s = settings_[i];

        lv_obj_t* row = lv_obj_create(list_);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, &st.row, LV_PART_MAIN);
        lv_obj_set_size(row, lv_pct(100), kRowH);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* nameLabel = lv_label_create(row);
        lv_obj_remove_style_all(nameLabel);
        lv_obj_add_style(nameLabel,
                         s.kind == SettingKind::Unimplemented ? &st.name_dim : &st.name,
                         LV_PART_MAIN);
        lv_label_set_text(nameLabel, s.label.c_str());
        lv_obj_align(nameLabel, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* valueLabel = lv_label_create(row);
        lv_obj_remove_style_all(valueLabel);
        lv_obj_add_style(valueLabel, s.editable() ? &st.value : &st.value_dim, LV_PART_MAIN);
        char valueText[64];
        formatValue(s, valueText, sizeof(valueText));
        lv_label_set_text(valueLabel, valueText);
        lv_obj_align(valueLabel, LV_ALIGN_RIGHT_MID, 0, 0);

        rows_.push_back(row);
        valueLabels_.push_back(valueLabel);
    }

    refreshSelection();
    LV_UNLOCK();
}

void UISettingsPage::refreshSelection() {
    if (rows_.empty())
        return;
    LV_LOCK();
    const RowStyles& st = rowStyles();

    // Only the outgoing and incoming rows are touched. Restyling every row on
    // every encoder detent would invalidate the whole list for a two-row
    // change, and this runs once per detent.
    if (styledRow_ >= 0 && styledRow_ < static_cast<int>(rows_.size()) && rows_[styledRow_]) {
        lv_obj_remove_style(rows_[styledRow_], &st.row_selected, LV_PART_MAIN);
        lv_obj_remove_style(rows_[styledRow_], &st.row_editing, LV_PART_MAIN);
    }
    styledRow_ = -1;

    if (selectedSetting_ >= 0 && selectedSetting_ < static_cast<int>(rows_.size()) &&
        rows_[selectedSetting_]) {
        lv_obj_t* row = rows_[selectedSetting_];
        lv_obj_add_style(row, editingValue_ ? &st.row_editing : &st.row_selected, LV_PART_MAIN);
        styledRow_ = selectedSetting_;
        // Keep the selection on screen: these lists scroll now, and an encoder
        // that walks off the bottom into nothing is the bug the old
        // fixed-height list had.
        lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    }
    LV_UNLOCK();
}

void UISettingsPage::moveSelection(int delta) {
    if (settings_.empty() || !hasEditableRow())
        return;

    const int n = static_cast<int>(settings_.size());
    int idx = selectedSetting_ < 0 ? (delta > 0 ? -1 : 0) : selectedSetting_;
    // Skip Info/Unimplemented rows: the encoder should never come to rest on
    // something it cannot change. hasEditableRow() guarantees termination.
    for (int step = 0; step < n; ++step) {
        idx = (idx + delta + n) % n;
        if (settings_[static_cast<size_t>(idx)].editable()) {
            break;
        }
    }
    selectedSetting_ = idx;
    editingValue_ = false;
    refreshSelection();

    ESP_LOGD(TAG,
             "Selection moved to %d: %s",
             selectedSetting_,
             settings_[static_cast<size_t>(selectedSetting_)].label.c_str());
}

void UISettingsPage::adjustValue(int delta) {
    if (selectedSetting_ < 0 || selectedSetting_ >= (int)settings_.size())
        return;

    auto& setting = settings_[selectedSetting_];
    if (!setting.editable())
        return;

    int newValue = setting.value + delta;

    if (newValue < setting.minValue)
        newValue = setting.minValue;
    if (newValue > setting.maxValue)
        newValue = setting.maxValue;

    if (newValue != setting.value) {
        updateSetting(selectedSetting_, newValue);
    }
}

void UISettingsPage::toggleEditMode() {
    if (selectedSetting_ < 0 || selectedSetting_ >= (int)settings_.size() ||
        !settings_[static_cast<size_t>(selectedSetting_)].editable()) {
        return;
    }
    editingValue_ = !editingValue_;
    refreshSelection();
    UINavigator::instance().refreshSoftkeys();

    ESP_LOGD(TAG,
             "Edit mode %s for setting: %s",
             editingValue_ ? "enabled" : "disabled",
             settings_[static_cast<size_t>(selectedSetting_)].label.c_str());
}

void UISettingsPage::updateSetting(int settingIndex, int newValue) {
    if (settingIndex < 0 || settingIndex >= (int)settings_.size())
        return;

    auto& setting = settings_[settingIndex];
    setting.value = newValue;

    if (settingIndex < (int)valueLabels_.size() && valueLabels_[settingIndex]) {
        LV_LOCK();
        char valueText[64];
        formatValue(setting, valueText, sizeof(valueText));
        lv_label_set_text(valueLabels_[settingIndex], valueText);
        LV_UNLOCK();
    }

    if (setting.onChange) {
        setting.onChange(newValue);
    }

    ESP_LOGI(TAG, "Setting '%s' changed to %d", setting.label.c_str(), newValue);
}

}  // namespace wavex_ui
