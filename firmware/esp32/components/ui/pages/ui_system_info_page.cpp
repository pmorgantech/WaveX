// WaveX Settings > System page - read-only facts about the frontend.
#include "ui/ui_system_info_page.h"

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "ui/ui_settings_page.h"

#include <cstdio>

namespace wavex_ui {

namespace {

const char* resetReasonText(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:
            return "power-on";
        case ESP_RST_EXT:
            return "external pin";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt watchdog";
        case ESP_RST_TASK_WDT:
            return "task watchdog";
        case ESP_RST_WDT:
            return "other watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep sleep wake";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "SDIO";
        default:
            return "unknown";
    }
}

void formatBytes(char* out, size_t out_len, size_t bytes) {
    if (bytes >= 1024u * 1024u) {
        snprintf(out, out_len, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        snprintf(out, out_len, "%u KB", static_cast<unsigned>(bytes / 1024u));
    }
}

class UISystemInfoPage : public UISettingsPage {
   public:
    UISystemInfoPage() : UISettingsPage("System") {
        const esp_app_desc_t* app = esp_app_get_description();
        esp_chip_info_t chip{};
        esp_chip_info(&chip);

        char buf[64];

        addInfo("Firmware version", app ? app->version : "unknown");
        if (app) {
            snprintf(buf, sizeof(buf), "%s %s", app->date, app->time);
            addInfo("Built", buf);
            addInfo("ESP-IDF", app->idf_ver);
        }

        snprintf(buf,
                 sizeof(buf),
                 "ESP32-P4 rev %d.%d, %d core%s",
                 chip.revision / 100,
                 chip.revision % 100,
                 chip.cores,
                 chip.cores == 1 ? "" : "s");
        addInfo("Chip", buf);
        addInfo("Last reset", resetReasonText(esp_reset_reason()));

        // Live rows, refreshed by the timer below. Indices are captured so the
        // refresh does not have to search the list by label.
        uptimeRow_ = static_cast<int>(settings_.size());
        addInfo("Uptime", "-");
        heapRow_ = static_cast<int>(settings_.size());
        addInfo("Internal heap free", "-");
        minHeapRow_ = static_cast<int>(settings_.size());
        addInfo("Internal heap low-water", "-");
        psramRow_ = static_cast<int>(settings_.size());
        addInfo("PSRAM free", "-");

        // Settings that would belong here but do not exist. Named rather than
        // omitted, so the absence is a statement instead of a gap someone has
        // to go read the source to explain.
        addUnimplemented("Save settings", "no settings are persisted (no NVS store yet)");
        addUnimplemented("Factory reset", "nothing stored to reset");
    }

    const char* name() const override { return "System"; }

    void onEnter(lv_obj_t* parent) override {
        UISettingsPage::onEnter(parent);
        refreshLive();
        // onEnter runs under the LVGL lock, so creating the timer here is safe.
        // 1 Hz: these are numbers a person reads, and the page is four labels.
        timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                static_cast<UISystemInfoPage*>(lv_timer_get_user_data(t))->refreshLive();
            },
            1000,
            this);
    }

    void onExit() override {
        // Before the widgets: a timer left running would fire against freed
        // labels the moment the tab or page went away.
        if (timer_) {
            lv_timer_del(timer_);
            timer_ = nullptr;
        }
        UISettingsPage::onExit();
    }

   private:
    lv_timer_t* timer_ = nullptr;
    int uptimeRow_ = -1;
    int heapRow_ = -1;
    int minHeapRow_ = -1;
    int psramRow_ = -1;

    void setRowText(int row, const char* text) {
        if (row < 0 || row >= static_cast<int>(settings_.size())) {
            return;
        }
        settings_[static_cast<size_t>(row)].text = text;
        if (row < static_cast<int>(valueLabels_.size()) && valueLabels_[static_cast<size_t>(row)]) {
            lv_label_set_text(valueLabels_[static_cast<size_t>(row)], text);
        }
    }

    void refreshLive() {
        char buf[64];

        const int64_t us = esp_timer_get_time();
        const int64_t secs = us / 1000000;
        snprintf(
            buf, sizeof(buf), "%lldh %02lldm %02llds", secs / 3600, (secs / 60) % 60, secs % 60);
        setRowText(uptimeRow_, buf);

        formatBytes(buf, sizeof(buf), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        setRowText(heapRow_, buf);
        formatBytes(buf, sizeof(buf), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        setRowText(minHeapRow_, buf);

        const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (psram == 0) {
            setRowText(psramRow_, "none");
        } else {
            formatBytes(buf, sizeof(buf), psram);
            setRowText(psramRow_, buf);
        }
    }
};

}  // namespace

std::shared_ptr<UIPage> createSystemInfoPage() {
    return std::make_shared<UISystemInfoPage>();
}

}  // namespace wavex_ui
