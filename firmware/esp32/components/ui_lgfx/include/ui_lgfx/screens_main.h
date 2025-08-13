#pragma once
#include "ui_lgfx/screen.h"

class MainMenuScreen : public Screen {
 public:
  void draw() override;
  void processEvent(const UIEvent& ev) override;
};

class SystemMenuScreen : public Screen {
 public:
  void draw() override;
  void processEvent(const UIEvent& ev) override;
};

class SettingsMenuScreen : public Screen {
 public:
  void draw() override;
  void processEvent(const UIEvent& ev) override;
};

class DiagnosticsScreen : public Screen {
 public:
  void draw() override;
  void processEvent(const UIEvent& ev) override;
  void updateMetrics(); // Called periodically to refresh system data
  bool needsAutoUpdate() const override { return true; } // This screen needs auto-update
  void updateContent() override; // Update only the dynamic content area
  
 private:
  // System metrics storage
  struct SystemMetrics {
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t total_heap;
    uint32_t uptime_seconds;
    uint8_t cpu_usage;
    uint32_t psram_free;
    uint32_t psram_total;
    uint32_t flash_total;
    uint32_t flash_used;
    uint32_t task_count;
    uint32_t last_update;
    
    // CPU monitoring (estimated)
    uint32_t cpu_measurement_start;
    uint32_t last_cpu_update;
  } metrics_;
  
  static const uint32_t UPDATE_INTERVAL_MS = 1000; // 1 Hz update rate for display refresh
  static const uint32_t CPU_MEASUREMENT_INTERVAL_MS = 1000; // 1 Hz CPU measurement (to avoid performance impact)
  uint32_t last_draw_update_ = 0;
  uint32_t last_cpu_update_ = 0;
};


