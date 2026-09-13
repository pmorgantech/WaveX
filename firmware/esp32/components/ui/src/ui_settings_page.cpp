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
// Design turn 3d: 72px rows on the card pitch, the value right-aligned in the
// mono face so a column of them lines up on the decimal point.
constexpr int kRowH = 72;
constexpr int kRowGap = UI_GUTTER;
constexpr int kListPad = UI_MARGIN_X;
constexpr int kValueLabelW = 420;

// Turn 4a: a tab whose settings are controls rather than information reads as
// a 2x2 tile grid, the same shape the Instrument's Filter stage uses. Applied
// only to small, editable tabs - Storage, MIDI and System are long lists of
// read-only facts, which a grid of four big tiles cannot carry.
constexpr size_t kTileLayoutMaxSettings = 4;
constexpr int kTileGap = 10;

// Shared styles: one set for the whole component, referenced by every row.
//
// Every lv_obj_set_style_*() call stores a property in the object's own style
// list, which allocates. A settings page builds up to a dozen rows of three
// objects each, and page entry is what this UI pays for (docs/roadmap.md), so
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
    lv_style_set_bg_color(&s.list, UI_COLOR_BG);
    lv_style_set_border_width(&s.list, 0);
    lv_style_set_radius(&s.list, 0);
    lv_style_set_pad_all(&s.list, kListPad);
    lv_style_set_pad_row(&s.list, kRowGap);
    lv_style_set_layout(&s.list, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s.list, LV_FLEX_FLOW_COLUMN);

    lv_style_init(&s.row);
    lv_style_set_bg_opa(&s.row, LV_OPA_COVER);
    lv_style_set_bg_color(&s.row, UI_COLOR_CARD);
    lv_style_set_border_width(&s.row, 1);
    lv_style_set_border_color(&s.row, UI_COLOR_LINE);
    lv_style_set_radius(&s.row, UI_RADIUS_CARD);
    lv_style_set_pad_left(&s.row, 24);
    lv_style_set_pad_right(&s.row, 24);

    // Selected / editing are additive overlays on top of `row`, so a selection
    // change is two add_style/remove_style calls rather than a rebuild.
    lv_style_init(&s.row_selected);
    lv_style_set_bg_color(&s.row_selected, UI_COLOR_CARD_ALT);
    lv_style_set_border_color(&s.row_selected, UI_COLOR_ACCENT);
    lv_style_set_border_width(&s.row_selected, UI_BORDER_WIDTH_FOCUS);

    lv_style_init(&s.row_editing);
    lv_style_set_bg_color(&s.row_editing, UI_COLOR_CARD_ALT);
    lv_style_set_border_color(&s.row_editing, UI_COLOR_WARN);
    lv_style_set_border_width(&s.row_editing, UI_BORDER_WIDTH_FOCUS);

    lv_style_init(&s.name);
    lv_style_set_text_font(&s.name, UI_FONT_BODY);
    lv_style_set_text_color(&s.name, UI_COLOR_FG);

    lv_style_init(&s.name_dim);
    lv_style_set_text_font(&s.name_dim, UI_FONT_BODY);
    lv_style_set_text_color(&s.name_dim, UI_COLOR_DIM);

    lv_style_init(&s.value);
    lv_style_set_text_font(&s.value, UI_FONT_MONO_SMALL);
    lv_style_set_text_color(&s.value, UI_COLOR_FG);
    lv_style_set_text_align(&s.value, LV_TEXT_ALIGN_RIGHT);
    lv_style_set_width(&s.value, kValueLabelW);

    lv_style_init(&s.value_dim);
    lv_style_set_text_font(&s.value_dim, UI_FONT_MONO_SMALL);
    lv_style_set_text_color(&s.value_dim, UI_COLOR_DIMMER);
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
        tiles_.clear();
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

// Four or fewer settings read as a 2x2 tile grid; more than that has to be a
// list. This is a capacity rule, not a taste one - four is what the content
// area holds at a size worth reading from a metre away, and System's eleven
// rows would lose most of themselves in it.
bool UISettingsPage::useTiles() const {
    return !settings_.empty() && settings_.size() <= kTileLayoutMaxSettings;
}

void UISettingsPage::rebuildList() {
    if (!list_)
        return;
    LV_LOCK();

    lv_obj_clean(list_);
    rows_.clear();
    valueLabels_.clear();
    tiles_.clear();
    styledRow_ = -1;

    if (useTiles()) {
        // Absolute placement inside the list, so the flex column the row
        // layout relies on has to come off first.
        lv_obj_set_style_layout(list_, LV_LAYOUT_NONE, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list_, 0, LV_PART_MAIN);
        lv_obj_remove_flag(list_, LV_OBJ_FLAG_SCROLLABLE);

        // Force layout before measuring: the list was sized moments ago and
        // its coordinates are not computed until LVGL next lays out, so
        // reading them here returns 0 and every tile comes out zero-sized -
        // which is a blank tab, not a visibly broken one.
        lv_obj_update_layout(list_);
        int32_t w = lv_obj_get_width(list_);
        int32_t h = lv_obj_get_height(list_);
        if (w <= 0 || h <= 0) {
            w = UI_CONTENT_WIDTH;
            h = UI_CONTENT_HEIGHT - UI_TAB_BAR_HEIGHT;
        }
        const int tw = (w - 2 * UI_MARGIN_X - kTileGap) / 2;
        const int th = (h - 2 * kTileGap - kTileGap) / 2;
        tiles_.resize(settings_.size());
        for (size_t i = 0; i < settings_.size(); ++i) {
            const Setting& st = settings_[i];
            const int col = static_cast<int>(i) % 2;
            const int row = static_cast<int>(i) / 2;
            tiles_[i] = valueTileCreate(list_,
                                        UI_MARGIN_X + col * (tw + kTileGap),
                                        kTileGap + row * (th + kTileGap),
                                        tw,
                                        th,
                                        st.label.c_str(),
                                        nullptr);
            char valueText[64];
            formatValue(st, valueText, sizeof(valueText));
            if (st.kind == SettingKind::Unimplemented) {
                // "NOT IMPLEMENTED" here, not the Instrument's "NOT WIRED":
                // these settings have no code behind them at all, where the
                // Instrument's inert controls are waiting on a protocol
                // message. The distinction is worth one word.
                valueTileSetUnwired(tiles_[i], st.text.c_str(), "NOT IMPLEMENTED");
            } else {
                if (st.editable()) {
                    const int idx = static_cast<int>(i);
                    valueTileSetOnAdjust(tiles_[i], [this, idx](int steps) {
                        selectedSetting_ = idx;
                        adjustValue(steps);
                        refreshSelection();
                    });
                }
                valueTileSetValue(tiles_[i], valueText, !st.editable());
                if (!st.desc.empty()) {
                    valueTileSetDesc(tiles_[i], st.desc.c_str());
                }
                const int span = st.maxValue - st.minValue;
                if (st.editable() && span > 0) {
                    valueTileSetFill(
                        tiles_[i],
                        static_cast<float>(st.value - st.minValue) / static_cast<float>(span));
                } else {
                    valueTileHideFill(tiles_[i]);
                }
            }
        }
        refreshSelection();
        LV_UNLOCK();
        return;
    }

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
    if (!tiles_.empty()) {
        LV_LOCK();
        for (size_t i = 0; i < tiles_.size(); ++i) {
            valueTileSetFocus(tiles_[i], static_cast<int>(i) == selectedSetting_);
        }
        LV_UNLOCK();
        return;
    }
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

    if (settingIndex < (int)tiles_.size() && tiles_[settingIndex].card) {
        LV_LOCK();
        char valueText[64];
        formatValue(setting, valueText, sizeof(valueText));
        valueTileSetValue(tiles_[settingIndex], valueText, !setting.editable());
        const int span = setting.maxValue - setting.minValue;
        valueTileSetFill(tiles_[settingIndex],
                         span > 0 ? static_cast<float>(setting.value - setting.minValue) /
                                        static_cast<float>(span)
                                  : 0.0f);
        LV_UNLOCK();
    } else if (settingIndex < (int)valueLabels_.size() && valueLabels_[settingIndex]) {
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
