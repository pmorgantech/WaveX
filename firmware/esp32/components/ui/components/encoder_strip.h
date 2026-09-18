#pragma once
#include <lvgl.h>

#include "ui/encoder_binding.h"
namespace wavex_ui {
constexpr int kEncoderStripHeight = 60;
class EncoderStrip {
   public:
    void Create(lv_obj_t* parent, int y);
    void Update(const EncoderBindings& bindings);
    void Reset() {
        labels_.fill(nullptr);
        values_.fill(nullptr);
    }

   private:
    std::array<lv_obj_t*, 4> labels_{}, values_{};
};
}  // namespace wavex_ui
