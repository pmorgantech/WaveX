// WaveX UI Sample Memory Diagnostics Page
#include "ui/ui_sample_memory_page.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "ui/ui_navigator.h"
#include "ui_task.h"

namespace wavex_ui {

namespace {
static const char* TAG = "UI_SAMPLE_MEM";
}  // namespace

void UISampleMemoryPage::onEnter(lv_obj_t* parent) {
    lv_obj_clean(parent);

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    ui_theme_apply_container_style(root_, true);
    lv_obj_set_style_pad_all(root_, UI_PADDING_MEDIUM, LV_PART_MAIN);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    summary_label_ = lv_label_create(root_);
    ui_theme_apply_label_style(summary_label_, false);
    lv_label_set_text(summary_label_, "Requesting sample memory status...");
    lv_obj_set_width(summary_label_, lv_pct(100));

    entries_label_ = lv_label_create(root_);
    ui_theme_apply_label_style(entries_label_, false);
    lv_label_set_text(entries_label_, "No samples loaded");
    lv_obj_set_width(entries_label_, lv_pct(100));
    lv_obj_set_style_text_align(entries_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);

    // Periodic refresh using LVGL timer (runs with LVGL lock held)
    refresh_timer_ = lv_timer_create(refresh_timer_cb, 1000, this);

    request_status();
    refresh_ui();
}

void UISampleMemoryPage::onExit() {
    if (refresh_timer_) {
        lv_timer_del(refresh_timer_);
        refresh_timer_ = nullptr;
    }
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        summary_label_ = nullptr;
        entries_label_ = nullptr;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UISampleMemoryPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    keys[1] = {"Refresh", [this]() {
                   request_status();
                   refresh_ui();
               }};
    return keys;
}

void UISampleMemoryPage::refresh_timer_cb(lv_timer_t* timer) {
    auto* page = static_cast<UISampleMemoryPage*>(lv_timer_get_user_data(timer));
    if (!page) {
        return;
    }
    page->request_status();
    page->refresh_ui();
}

void UISampleMemoryPage::request_status() {
    esp_err_t res = inter_mcu_request_sample_mem_status();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "Failed to request sample memory status: %d", res);
    }
}

void UISampleMemoryPage::format_bytes(uint32_t bytes, char* out, size_t len) {
    if (!out || len == 0) {
        return;
    }
    if (bytes >= 1024 * 1024) {
        snprintf(out, len, "%.2f MB", bytes / (1024.0f * 1024.0f));
    } else if (bytes >= 1024) {
        snprintf(out, len, "%.1f KB", bytes / 1024.0f);
    } else {
        snprintf(out, len, "%lu B", static_cast<unsigned long>(bytes));
    }
}

void UISampleMemoryPage::refresh_ui() {
    inter_mcu_get_sample_mem_status(&status_cache_);

    // Summary text
    char small_total[32], small_free[32], large_total[32], large_free[32], largest_free[32],
        in_use[32];
    format_bytes(status_cache_.small_total_bytes, small_total, sizeof(small_total));
    format_bytes(status_cache_.small_free_bytes, small_free, sizeof(small_free));
    format_bytes(status_cache_.large_total_bytes, large_total, sizeof(large_total));
    format_bytes(status_cache_.large_free_bytes, large_free, sizeof(large_free));
    format_bytes(status_cache_.largest_free_bytes, largest_free, sizeof(largest_free));
    format_bytes(status_cache_.in_use_bytes, in_use, sizeof(in_use));

    char summary[512];
    snprintf(summary,
             sizeof(summary),
             "Small Pool: %s total / %s free\n"
             "Large Pool: %s total / %s free\n"
             "Largest Free Extent: %s\n"
             "In Use: %s\n"
             "Failed Allocs: %lu",
             small_total,
             small_free,
             large_total,
             large_free,
             largest_free,
             in_use,
             static_cast<unsigned long>(status_cache_.failed_allocs));

    if (summary_label_) {
        lv_label_set_text(summary_label_, summary);
    }

    // Entries text
    char entries[1024];
    size_t offset = 0;
    if (status_cache_.sample_count == 0) {
        offset += snprintf(entries + offset,
                           sizeof(entries) - offset,
                           "Loaded Samples:\n- None (no allocations yet)");
    } else {
        offset += snprintf(entries + offset, sizeof(entries) - offset, "Loaded Samples:\n");
        for (uint8_t i = 0; i < status_cache_.sample_count && offset < sizeof(entries); ++i) {
            const auto& entry = status_cache_.entries[i];

            char alloc[32], loaded[32];
            format_bytes(entry.allocated_bytes, alloc, sizeof(alloc));
            format_bytes(entry.loaded_bytes, loaded, sizeof(loaded));

            const char* pool = (entry.cls == 0xFF) ? "Large" : "Small";
            offset += snprintf(entries + offset,
                               sizeof(entries) - offset,
                               "- ID %u: %s (%s loaded)\n  Pool: %s cls=%u page=%u slot=%u\n"
                               "  %u Hz, %u ch, %u-bit\n",
                               (unsigned)entry.sample_id,
                               alloc,
                               loaded,
                               pool,
                               (unsigned)entry.cls,
                               (unsigned)entry.page,
                               (unsigned)entry.slot,
                               (unsigned)entry.sample_rate,
                               (unsigned)entry.channels,
                               (unsigned)entry.bit_depth);
        }
    }

    if (entries_label_) {
        lv_label_set_text(entries_label_, entries);
    }

    wavex_ui_mark_content_changed();
}

std::shared_ptr<UIPage> createSampleMemoryPage() {
    return std::make_shared<UISampleMemoryPage>();
}

}  // namespace wavex_ui

