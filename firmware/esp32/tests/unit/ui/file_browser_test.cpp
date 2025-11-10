#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Include mock headers to get type definitions
#include "file_browser.h"
#include "lvgl.h"
#include "ui_theme.h"

// Mock inter-MCU functions (implementations are in ui_mocks.cpp, but we need declarations here)
extern "C" {
void wavex_ui_mark_content_changed() {}
}

// Mock ESP-IDF functions
extern "C" {
void esp_log_write(int level, const char* tag, const char* format, ...) {
    (void)level;
    (void)tag;
    (void)format;
}
int64_t esp_timer_get_time(void) {
    static int64_t t = 0;
    return t++;
}
}

// Mock ESP-LVGL port
extern "C" {
void esp_lvgl_port_lock(void) {}
void esp_lvgl_port_unlock(void) {}
}

// Mock FreeRTOS
#include "freertos/FreeRTOS.h"

// Mock UI theme constants
#define UI_COLOR_CONTENT {0, 0, 0}
#define UI_PADDING_SMALL 4
#define UI_PADDING_MEDIUM 8
#define LV_ALIGN_TOP_LEFT 0
#define LV_PART_MAIN 0

class FileBrowserTest : public ::testing::Test {
   protected:
    void SetUp() override {
        // Reset any global state if needed
    }

    void TearDown() override {
        // Cleanup handled by test
    }

    // Helper to create a minimal browser structure for testing navigation logic
    wavex_file_browser_t* CreateMinimalBrowser(const char* path, uint32_t entry_count) {
        wavex_file_browser_t* browser = (wavex_file_browser_t*)malloc(sizeof(wavex_file_browser_t));
        if (!browser)
            return nullptr;

        memset(browser, 0, sizeof(wavex_file_browser_t));
        strncpy(browser->current_path, path, sizeof(browser->current_path) - 1);
        browser->current_path[sizeof(browser->current_path) - 1] = '\0';

        browser->entry_count = entry_count;
        browser->selected_index = 0;
        browser->first_visible_index = 0;
        browser->visible_count = 8;

        // Allocate entries
        browser->entries = (wavex_file_entry_t*)malloc(entry_count * sizeof(wavex_file_entry_t));
        if (browser->entries) {
            for (uint32_t i = 0; i < entry_count; i++) {
                snprintf(
                    browser->entries[i].name, sizeof(browser->entries[i].name), "file%02u.wav", i);
                snprintf(browser->entries[i].path,
                         sizeof(browser->entries[i].path),
                         "%s/file%02u.wav",
                         path,
                         i);
                browser->entries[i].is_directory = false;
                browser->entries[i].size_bytes = 1000 + i;
            }
        }

        return browser;
    }

    void DestroyBrowser(wavex_file_browser_t* browser) {
        if (browser) {
            if (browser->entries) {
                free(browser->entries);
            }
            free(browser);
        }
    }
};

// Test: Navigate up from subdirectory
TEST_F(FileBrowserTest, NavigateUpFromSubdirectory) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/SOUNDS", 5);
    ASSERT_NE(browser, nullptr);

    // Navigation should update the path even if browse request fails
    wavex_file_browser_navigate_up(browser);

    // Check that path was updated correctly (browse request may fail in test environment)
    EXPECT_STREQ("/", browser->current_path);

    DestroyBrowser(browser);
}

// Test: Navigate up from root (should fail)
TEST_F(FileBrowserTest, NavigateUpFromRoot) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 5);
    ASSERT_NE(browser, nullptr);

    bool result = wavex_file_browser_navigate_up(browser);

    EXPECT_FALSE(result);
    EXPECT_STREQ("/", browser->current_path);  // Should remain at root

    DestroyBrowser(browser);
}

// Test: Navigate up from nested directory
TEST_F(FileBrowserTest, NavigateUpFromNestedDirectory) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/SOUNDS/DRUMS", 5);
    ASSERT_NE(browser, nullptr);

    // Navigation should update the path even if browse request fails
    wavex_file_browser_navigate_up(browser);

    // Check that path was updated correctly (browse request may fail in test environment)
    EXPECT_STREQ("/SOUNDS", browser->current_path);

    DestroyBrowser(browser);
}

// Test: Navigate down entry (move selection down)
TEST_F(FileBrowserTest, NavigateDownEntry) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 10);
    ASSERT_NE(browser, nullptr);
    browser->selected_index = 0;

    bool result = wavex_file_browser_navigate_down_entry(browser);

    EXPECT_TRUE(result);
    EXPECT_EQ(1u, browser->selected_index);

    DestroyBrowser(browser);
}

// Test: Navigate down entry at last position (should fail)
TEST_F(FileBrowserTest, NavigateDownEntryAtLast) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 10);
    ASSERT_NE(browser, nullptr);
    browser->selected_index = 9;  // Last entry

    bool result = wavex_file_browser_navigate_down_entry(browser);

    EXPECT_FALSE(result);
    EXPECT_EQ(9u, browser->selected_index);  // Should remain at last

    DestroyBrowser(browser);
}

// Test: Navigate up entry (move selection up)
TEST_F(FileBrowserTest, NavigateUpEntry) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 10);
    ASSERT_NE(browser, nullptr);
    browser->selected_index = 5;

    bool result = wavex_file_browser_navigate_up_entry(browser);

    EXPECT_TRUE(result);
    EXPECT_EQ(4u, browser->selected_index);

    DestroyBrowser(browser);
}

// Test: Navigate up entry at first position (should fail)
TEST_F(FileBrowserTest, NavigateUpEntryAtFirst) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 10);
    ASSERT_NE(browser, nullptr);
    browser->selected_index = 0;

    bool result = wavex_file_browser_navigate_up_entry(browser);

    EXPECT_FALSE(result);
    EXPECT_EQ(0u, browser->selected_index);  // Should remain at first

    DestroyBrowser(browser);
}

// Test: Navigate down entry with empty browser (should fail)
TEST_F(FileBrowserTest, NavigateDownEntryEmptyBrowser) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 0);
    ASSERT_NE(browser, nullptr);

    bool result = wavex_file_browser_navigate_down_entry(browser);

    EXPECT_FALSE(result);

    DestroyBrowser(browser);
}

// Test: Set selection with valid index
TEST_F(FileBrowserTest, SetSelectionValidIndex) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 10);
    ASSERT_NE(browser, nullptr);

    wavex_file_browser_set_selection(browser, 5);

    EXPECT_EQ(5u, browser->selected_index);

    DestroyBrowser(browser);
}

// Test: Set selection with invalid index (should be ignored)
TEST_F(FileBrowserTest, SetSelectionInvalidIndex) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 10);
    ASSERT_NE(browser, nullptr);
    browser->selected_index = 0;

    wavex_file_browser_set_selection(browser, 20);  // Invalid index

    EXPECT_EQ(0u, browser->selected_index);  // Should remain unchanged

    DestroyBrowser(browser);
}

// Test: Get selected entry
TEST_F(FileBrowserTest, GetSelectedEntry) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 10);
    ASSERT_NE(browser, nullptr);
    browser->selected_index = 3;

    const wavex_file_entry_t* entry = wavex_file_browser_get_selected(browser);

    ASSERT_NE(entry, nullptr);
    EXPECT_STREQ("file03.wav", entry->name);

    DestroyBrowser(browser);
}

// Test: Get selected index
TEST_F(FileBrowserTest, GetSelectedIndex) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 10);
    ASSERT_NE(browser, nullptr);
    browser->selected_index = 7;

    uint32_t index = wavex_file_browser_get_selected_index(browser);

    EXPECT_EQ(7u, index);

    DestroyBrowser(browser);
}

// Test: Viewport scrolling when selection moves above visible area
TEST_F(FileBrowserTest, ViewportScrollUp) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 20);
    ASSERT_NE(browser, nullptr);
    browser->selected_index = 10;
    browser->first_visible_index = 10;
    browser->visible_count = 8;

    // Move selection up above visible area
    browser->selected_index = 5;
    wavex_file_browser_set_selection(browser, 5);

    EXPECT_EQ(5u, browser->first_visible_index);  // Viewport should scroll up

    DestroyBrowser(browser);
}

// Test: Viewport scrolling when selection moves below visible area
TEST_F(FileBrowserTest, ViewportScrollDown) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 20);
    ASSERT_NE(browser, nullptr);
    browser->selected_index = 5;
    browser->first_visible_index = 5;
    browser->visible_count = 8;

    // Move selection down below visible area
    browser->selected_index = 15;
    wavex_file_browser_set_selection(browser, 15);

    // Viewport should scroll down to show selected entry
    uint32_t expected_first_visible = 15 - (browser->visible_count - 1);
    EXPECT_EQ(expected_first_visible, browser->first_visible_index);

    DestroyBrowser(browser);
}

// Test: Navigate to path
TEST_F(FileBrowserTest, NavigateToPath) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 5);
    ASSERT_NE(browser, nullptr);

    bool result = wavex_file_browser_navigate_to(browser, "/SOUNDS");

    // Note: This will fail because refresh_file_list needs inter-MCU communication
    // But we can test the path update logic
    EXPECT_STREQ("/SOUNDS", browser->current_path);

    DestroyBrowser(browser);
}

// Test: Multiple navigate down operations
TEST_F(FileBrowserTest, MultipleNavigateDown) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 10);
    ASSERT_NE(browser, nullptr);
    browser->selected_index = 0;

    // Navigate down 5 times
    for (int i = 0; i < 5; i++) {
        bool result = wavex_file_browser_navigate_down_entry(browser);
        EXPECT_TRUE(result);
        EXPECT_EQ((uint32_t)(i + 1), browser->selected_index);
    }

    EXPECT_EQ(5u, browser->selected_index);

    DestroyBrowser(browser);
}

// Test: Multiple navigate up operations
TEST_F(FileBrowserTest, MultipleNavigateUp) {
    wavex_file_browser_t* browser = CreateMinimalBrowser("/", 10);
    ASSERT_NE(browser, nullptr);
    browser->selected_index = 9;

    // Navigate up 5 times
    for (int i = 0; i < 5; i++) {
        bool result = wavex_file_browser_navigate_up_entry(browser);
        EXPECT_TRUE(result);
        EXPECT_EQ((uint32_t)(9 - i - 1), browser->selected_index);
    }

    EXPECT_EQ(4u, browser->selected_index);

    DestroyBrowser(browser);
}
