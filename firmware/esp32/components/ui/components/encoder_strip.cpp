#include "encoder_strip.h"

#include "ui_theme.h"

#include <cstdio>
#include <cstring>
namespace wavex_ui {
void EncoderStrip::Create(lv_obj_t* parent, int y) {
    Reset();
    constexpr int width = (UI_SCREEN_WIDTH - 2 * UI_MARGIN_X) / 4;
    for (size_t i = 0; i < 4; ++i) {
        auto* card = lv_obj_create(parent);
        lv_obj_remove_style_all(card);
        lv_obj_set_pos(card, UI_MARGIN_X + static_cast<int>(i) * width, y);
        lv_obj_set_size(card, width - UI_PADDING_SMALL, kEncoderStripHeight);
        lv_obj_set_style_bg_color(card, UI_COLOR_CARD_ALT, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        labels_[i] = lv_label_create(card);
        values_[i] = lv_label_create(card);
        lv_obj_set_style_text_font(labels_[i], UI_FONT_MICRO, 0);
        lv_obj_set_style_text_font(values_[i], UI_FONT_MONO_SMALL, 0);
        lv_obj_set_pos(labels_[i], UI_PADDING_SMALL, 4);
        lv_obj_set_pos(values_[i], UI_PADDING_SMALL, 26);
        for (auto* label: {labels_[i], values_[i]}) {
            lv_obj_set_width(label, width - 3 * UI_PADDING_SMALL);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        }
    }
    Update({});
}
void EncoderStrip::Update(const EncoderBindings& bindings) {
    for (size_t i = 0; i < 4; ++i) {
        if (!labels_[i])
            continue;
        const auto& binding = bindings[i];
        char label[48];
        std::snprintf(label, sizeof(label), "%u  %s", static_cast<unsigned>(i + 1), binding.label);
        if (std::strcmp(lv_label_get_text(labels_[i]), label))
            lv_label_set_text(labels_[i], label);
        const char* value = binding.value[0] ? binding.value.data() : "--";
        if (std::strcmp(lv_label_get_text(values_[i]), value))
            lv_label_set_text(values_[i], value);
        auto color = binding.enabled ? UI_COLOR_FG : UI_COLOR_DIMMER;
        if (!lv_color_eq(lv_obj_get_style_text_color(values_[i], LV_PART_MAIN), color))
            lv_obj_set_style_text_color(values_[i], color, 0);
        if (!lv_color_eq(lv_obj_get_style_text_color(labels_[i], LV_PART_MAIN), UI_COLOR_DIM))
            lv_obj_set_style_text_color(labels_[i], UI_COLOR_DIM, 0);
    }
}
}  // namespace wavex_ui
