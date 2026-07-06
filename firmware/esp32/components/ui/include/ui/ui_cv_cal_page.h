// WaveX CV Calibration Page (roadmap item 5 stage 5)
#pragma once

#include "ui_page.h"

#include <memory>

namespace wavex_ui {

// Walks the Stage A CV calibration procedure (analog-voice-board.md §3):
// adjust CvCal gain/offset/curvature per control (values apply live on the
// Daisy), drive steady test CVs for corner-frequency measurement and
// VCA-silence verification, and persist the table to the Daisy's SD card.
std::shared_ptr<UIPage> createCvCalPage();

}  // namespace wavex_ui
