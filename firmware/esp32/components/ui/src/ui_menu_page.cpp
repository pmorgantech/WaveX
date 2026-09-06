// WaveX UI Menu Page Implementation
#include "ui/ui_menu_page.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "esp_lvgl_port.h"

// LVGL locking macros
#define LV_LOCK() lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

static const char* TAG = "UI_MENU_PAGE";

namespace wavex_ui {
namespace {

// Row geometry. Five rows at this pitch occupy 472 of the 557px content area,
// which leaves the list top-aligned with room to grow rather than centred and
// reflowing every time an entry is added.
constexpr int kListTopPad = 17;
constexpr int kRowH = 88;
constexpr int kRowPadX = 24;
constexpr int kRailW = 6;
constexpr int kRailH = 44;
constexpr int kDotSize = 10;

}  // namespace

void UIMenuPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // Title is rendered by navigator header; omit internal title

    // Not lv_list: the design's row is a card with a focus rail, a title, a
    // purpose line and a live context readout, which is more structure than a
    // list button holds. A plain flex column of cards also lets a selection
    // change restyle two rows instead of rebuilding all of them.
    list_ = lv_obj_create(root_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_size(list_, UI_SCREEN_WIDTH - 2 * UI_MARGIN_X, lv_pct(100));
    lv_obj_set_pos(list_, UI_MARGIN_X, kListTopPad);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_, UI_GUTTER, 0);
    lv_obj_remove_flag(list_, LV_OBJ_FLAG_SCROLLABLE);

    rebuildList();

    // One second is fast enough for "is the link up" and slow enough that the
    // menu costs nothing to leave sitting on screen. Each tick writes a label
    // only when its text actually changed, so a steady state does not
    // invalidate the row at all.
    context_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<UIMenuPage*>(lv_timer_get_user_data(t))->refreshContext();
        },
        1000,
        this);
}

void UIMenuPage::onExit() {
    if (context_timer_) {
        lv_timer_delete(context_timer_);
        context_timer_ = nullptr;
    }
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        list_ = nullptr;
    }
    // The Row pointers belong to the tree just deleted; keeping them would
    // leave applyRowState() writing styles into freed objects on re-entry.
    rows_.clear();
}

void UIMenuPage::onInput(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::EncoderLeft:
            moveSelection(-1);
            break;
        case InputType::EncoderRight:
            moveSelection(+1);
            break;
        case InputType::ButtonPress:
        case InputType::EncoderClick:
            activateSelection();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UIMenuPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};

    if (UINavigator::instance().canPop()) {
        keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    }

    keys[1] = {"Select", [this]() { activateSelection(); }};

    return keys;
}

void UIMenuPage::rebuildList() {
    if (!list_)
        return;
    lv_obj_clean(list_);
    rows_.clear();
    rows_.resize(items_.size());

    for (size_t i = 0; i < items_.size(); ++i) {
        const MenuItem& item = items_[i];
        Row& row = rows_[i];

        row.card = lv_obj_create(list_);
        lv_obj_remove_style_all(row.card);
        lv_obj_set_size(row.card, lv_pct(100), kRowH);
        // A menu row is a button, so it is drawn as one: a lifted fill and an
        // edge you can see against the page rather than the hairline a
        // read-only card gets. On this panel the card/line pair is only a few
        // RGB565 steps off the background, which reads as a list, not as five
        // things you can press.
        lv_obj_set_style_bg_color(row.card, UI_COLOR_CARD_ALT, 0);
        lv_obj_set_style_bg_opa(row.card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(
            row.card, UI_COLOR_CARD, static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
        lv_obj_set_style_radius(row.card, UI_RADIUS_BUTTON, 0);
        lv_obj_set_style_pad_hor(row.card, kRowPadX, 0);
        lv_obj_remove_flag(row.card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row.card, LV_OBJ_FLAG_CLICKABLE);

        // A rail rather than a fill change for focus: the row keeps its own
        // colour, so a focused row and a row you are simply reading do not
        // swap which one looks "on".
        row.rail = lv_obj_create(row.card);
        lv_obj_remove_style_all(row.rail);
        lv_obj_set_size(row.rail, kRailW, kRailH);
        lv_obj_align(row.rail, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_opa(row.rail, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row.rail, kRailW / 2, 0);

        lv_obj_t* title = lv_label_create(row.card);
        lv_label_set_text(title, item.label.c_str());
        lv_obj_set_style_text_font(title, UI_FONT_HEADING, 0);
        lv_obj_set_style_text_color(title, UI_COLOR_FG, 0);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, kRailW + 20, item.purpose.empty() ? 0 : -15);

        if (!item.purpose.empty()) {
            lv_obj_t* purpose = lv_label_create(row.card);
            lv_label_set_text(purpose, item.purpose.c_str());
            lv_obj_set_style_text_font(purpose, UI_FONT_SMALL, 0);
            lv_obj_set_style_text_color(purpose, UI_COLOR_DIM, 0);
            lv_obj_align(purpose, LV_ALIGN_LEFT_MID, kRailW + 20, 16);
        }

        lv_obj_t* chevron = lv_label_create(row.card);
        // LV_SYMBOL_RIGHT, not a typographic chevron: the Montserrat tables
        // are built over printable ASCII, so U+203A would render as a box.
        lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chevron, UI_COLOR_DIMMER, 0);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);

        if (item.context) {
            row.context = lv_label_create(row.card);
            lv_label_set_text(row.context, "");
            lv_obj_set_style_text_font(row.context, UI_FONT_MONO_SMALL, 0);
            lv_obj_set_style_text_color(row.context, UI_COLOR_DIM, 0);
            lv_obj_align(row.context, LV_ALIGN_RIGHT_MID, -28, 0);
        }
        if (item.ok) {
            row.dot = lv_obj_create(row.card);
            lv_obj_remove_style_all(row.dot);
            lv_obj_set_size(row.dot, kDotSize, kDotSize);
            lv_obj_set_style_radius(row.dot, kDotSize / 2, 0);
            lv_obj_set_style_bg_opa(row.dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row.dot, UI_COLOR_DIMMER, 0);
        }

        lv_obj_add_event_cb(
            row.card,
            [](lv_event_t* e) {
                auto* self = static_cast<UIMenuPage*>(lv_event_get_user_data(e));
                lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
                for (size_t j = 0; j < self->rows_.size(); ++j) {
                    if (self->rows_[j].card != target) {
                        continue;
                    }
                    self->selected_ = static_cast<int>(j);
                    // Deferred: activating pushes a page, which deletes this
                    // row while its own event is still being dispatched.
                    lv_async_call(
                        [](void* ud) { static_cast<UIMenuPage*>(ud)->activateSelection(); }, self);
                    break;
                }
            },
            LV_EVENT_SHORT_CLICKED,
            this);

        applyRowState(i);
    }
    refreshContext();
}

// Focus styling for one row. Split out so moving the selection touches only
// the row losing focus and the row gaining it.
void UIMenuPage::applyRowState(size_t index) {
    if (index >= rows_.size() || !rows_[index].card) {
        return;
    }
    const bool on = static_cast<int>(index) == selected_;
    Row& row = rows_[index];
    lv_obj_set_style_border_width(
        row.card, on ? UI_BORDER_WIDTH_FOCUS : UI_BORDER_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_border_color(row.card, on ? UI_COLOR_ACCENT : UI_COLOR_EDGE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row.rail, on ? UI_COLOR_ACCENT : UI_COLOR_LINE, 0);
}

// Polled once a second while the menu is on screen. Writes only on change, so
// a menu nobody is touching costs one comparison per row per second and no
// invalidation at all.
void UIMenuPage::refreshContext() {
    for (size_t i = 0; i < rows_.size() && i < items_.size(); ++i) {
        Row& row = rows_[i];
        if (row.context && items_[i].context) {
            std::string text = items_[i].context();
            if (text != row.last_context) {
                row.last_context = text;
                lv_label_set_text(row.context, text.c_str());
                if (row.dot) {
                    lv_obj_update_layout(row.context);
                    lv_obj_align_to(
                        row.dot, row.context, LV_ALIGN_OUT_LEFT_MID, -UI_PADDING_MEDIUM, 0);
                }
            }
        }
        if (row.dot && items_[i].ok) {
            const int ok = items_[i].ok() ? 1 : 0;
            if (ok != row.last_ok) {
                row.last_ok = ok;
                lv_obj_set_style_bg_color(row.dot, ok ? UI_COLOR_OK : UI_COLOR_WARN, 0);
            }
        }
    }
}

// Debug harness: the highlighted item, so a host can steer with the encoder
// from wherever the highlight was left rather than guessing.
size_t UIMenuPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "items", static_cast<long>(items_.size()));
    const bool valid = selected_ >= 0 && selected_ < static_cast<int>(items_.size());
    len = AppendKvInt(out, cap, len, "selidx", valid ? selected_ : -1);
    return AppendKvText(
        out, cap, len, "sel", valid ? items_[static_cast<size_t>(selected_)].label.c_str() : "-");
}

void UIMenuPage::moveSelection(int delta) {
    if (items_.empty())
        return;

    const size_t previous = static_cast<size_t>(selected_);
    selected_ = static_cast<int>((selected_ + delta + items_.size()) % items_.size());
    // Two rows restyled, not the whole list rebuilt. The old rebuild ran
    // lv_obj_clean() plus a full reconstruction on every encoder detent, which
    // invalidated the entire content area to move one highlight.
    applyRowState(previous);
    applyRowState(static_cast<size_t>(selected_));

    ESP_LOGD(TAG, "Selection moved to %d: %s", selected_, items_[selected_].label.c_str());
}

void UIMenuPage::activateSelection() {
    if (selected_ >= 0 && selected_ < (int)items_.size()) {
        ESP_LOGI(TAG, "Activating menu item: %s", items_[selected_].label.c_str());
        if (items_[selected_].onSelect) {
            items_[selected_].onSelect();
        }
    }
}

// Retained for the case where a click lands on the list container rather than
// on a row - the per-row handler above is what normally fires.
void UIMenuPage::list_event_cb(lv_event_t* e) {
    auto* self = static_cast<UIMenuPage*>(lv_event_get_user_data(e));
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));

    for (size_t i = 0; i < self->rows_.size(); ++i) {
        if (self->rows_[i].card != target) {
            continue;
        }
        self->selected_ = static_cast<int>(i);
        lv_async_call([](void* ud) { static_cast<UIMenuPage*>(ud)->activateSelection(); }, self);
        break;
    }
}

}  // namespace wavex_ui
