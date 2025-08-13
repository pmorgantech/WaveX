#include "ui_lgfx/screens_display.h"
#include "ui_lgfx/lgfx_device.h"
#include "ui_lgfx/ui_manager.h"
#include "ui_lgfx/screensaver.h"
#include <LovyanGFX.hpp>

extern "C" lgfx::LGFX_Device* lgfxInternal();
static inline lgfx::LGFX_Device& lcdRef() { return *lgfxInternal(); }

// Define static arrays for screensaver options
const int DisplaySettingsScreen::SCREENSAVER_OPTIONS[] = {1, 2, 5, 10, 30, 60, 0}; // 0 = never
const char* const DisplaySettingsScreen::SCREENSAVER_LABELS[] = {"1m", "2m", "5m", "10m", "30m", "60m", "Never"};

void DisplaySettingsScreen::draw() {
  auto &lcd = lcdRef();
  lcd.fillScreen(0x0000);
  lcd.fillRect(0, 0, 480, 24, 0x7BEF);
  lcd.setTextColor(0xFFFF, 0x7BEF);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.drawString("Display Settings", 240, 12);

  // Brightness slider area
  lcd.setTextColor(0xFFFF, 0x0000);
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.drawString("Brightness", 20, 50);
  int barX = 20, barY = 80, barW = 440, barH = 16;
  // Clear bar interior then draw border and fill according to brightness
  lcd.fillRect(barX, barY, barW, barH, 0x0000);
  lcd.drawRect(barX, barY, barW, barH, 0xFFFF);
  int fillW = ((brightness_ - MIN_BRIGHTNESS) * barW) / (MAX_BRIGHTNESS - MIN_BRIGHTNESS);
  if (fillW > 2) {
    lcd.fillRect(barX + 1, barY + 1, fillW - 2, barH - 2, 0x07E0);
  }
  
  // Screensaver setting
  lcd.drawString("Screensaver", 20, 120);
  lcd.drawString(SCREENSAVER_LABELS[screensaver_minutes_ == 0 ? 6 : 
    (screensaver_minutes_ == 1 ? 0 : 
     screensaver_minutes_ == 2 ? 1 : 
     screensaver_minutes_ == 5 ? 2 : 
     screensaver_minutes_ == 10 ? 3 : 
     screensaver_minutes_ == 30 ? 4 : 
     screensaver_minutes_ == 60 ? 5 : 6)], 200, 120);

  // Bottom softkeys
  lcd.fillRect(0, 296, 480, 24, 0x7BEF);
  lcd.setTextColor(0xFFFF, 0x7BEF);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.drawString("BRIGHT", 80, 308);
  lcd.drawString("BACK", 240, 308);
  lcd.drawString("SAVER", 400, 308);
}

void DisplaySettingsScreen::processEvent(const UIEvent& ev) {
  if (ev.type != UIEvent::TOUCH) return;

  // Softkey bar
  if (ev.y >= 296) {
    if (ev.x < 160) {
      // Cycle through brightness presets
      if (brightness_ >= 200) brightness_ = 179;      // 100% -> 70%
      else if (brightness_ >= 179) brightness_ = 128; // 70% -> 50%
      else if (brightness_ >= 128) brightness_ = 64;  // 50% -> 25%
      else brightness_ = 200;                          // 25% -> 100%
      lgfx_device_set_brightness(brightness_);
      draw();
      return;
    }
    if (ev.x < 320) {
      // Back: pop current screen
      UIManager::instance().pop();
      return;
    }
    // Right softkey: cycle screensaver time
    int current_idx = 0;
    for (int i = 0; i < 7; i++) {
      if (SCREENSAVER_OPTIONS[i] == screensaver_minutes_) {
        current_idx = i;
        break;
      }
    }
    current_idx = (current_idx + 1) % 7;
    screensaver_minutes_ = SCREENSAVER_OPTIONS[current_idx];
    // Update screensaver settings
    screensaver_set_timeout(screensaver_minutes_);
    draw();
    return;
  }

  // Direct tap on brightness bar adjusts proportionally
  int barX = 20, barY = 80, barW = 440, barH = 16;
  if (ev.x >= barX && ev.x < barX + barW && ev.y >= barY && ev.y < barY + barH) {
    int new_b = MIN_BRIGHTNESS + (int)((int64_t)(ev.x - barX) * (MAX_BRIGHTNESS - MIN_BRIGHTNESS) / barW);
    if (new_b < 0) {
      new_b = 0;
    }
    if (new_b > 255) {
      new_b = 255;
    }
    // Clamp to safe range
    if (new_b < MIN_BRIGHTNESS) new_b = MIN_BRIGHTNESS;
    if (new_b > MAX_BRIGHTNESS) new_b = MAX_BRIGHTNESS;
    if (new_b != brightness_) {
      brightness_ = new_b;
      lgfx_device_set_brightness(brightness_);
      draw();
    }
  }
  
  // Tap on screensaver area to cycle through options
  if (ev.x >= 200 && ev.x < 350 && ev.y >= 120 && ev.y < 140) {
    int current_idx = 0;
    for (int i = 0; i < 7; i++) {
      if (SCREENSAVER_OPTIONS[i] == screensaver_minutes_) {
        current_idx = i;
        break;
      }
    }
    current_idx = (current_idx + 1) % 7;
    screensaver_minutes_ = SCREENSAVER_OPTIONS[current_idx];
    screensaver_set_timeout(screensaver_minutes_);
    draw();
  }
}


