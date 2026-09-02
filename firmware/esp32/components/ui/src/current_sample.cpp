#include "ui/current_sample.h"

namespace wavex_ui {

namespace {
uint16_t s_current_sample_id = 0;
}  // namespace

uint16_t getCurrentSampleId() {
    return s_current_sample_id;
}

void setCurrentSampleId(uint16_t sample_id) {
    s_current_sample_id = sample_id;
}

}  // namespace wavex_ui
