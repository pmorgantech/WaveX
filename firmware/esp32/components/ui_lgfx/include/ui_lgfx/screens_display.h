#pragma once
#include "ui_lgfx/screen.h"

class DisplaySettingsScreen : public Screen {
 public:
  void draw() override;
  void processEvent(const UIEvent& ev) override;
 private:
  int brightness_ = 179; // Default to 70% (179/255)
  int screensaver_minutes_ = 5; // Default 5 minutes
  static const int MIN_BRIGHTNESS = 16; // Prevent display from going completely dark
  static const int MAX_BRIGHTNESS = 255;
  static const int SCREENSAVER_OPTIONS[7]; // 1, 2, 5, 10, 30, 60, 0 (0 = never)
  static const char* const SCREENSAVER_LABELS[7]; // "1m", "2m", "5m", "10m", "30m", "60m", "Never"
};


