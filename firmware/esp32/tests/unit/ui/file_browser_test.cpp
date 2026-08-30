#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Include mock headers to get type definitions
#include "../../mocks/esp32_mocks.h"
#include "comm/comm_interface_impl.h"
#include "comm/statistics.h"
#include "file_browser.h"
#include "lvgl.h"
#include "spi_protocol/protocol.h"
#include "ui_theme.h"

// wavex_ui_mark_content_changed and esp_timer_get_time are provided by
// ui_mocks.cpp / esp32_mocks.cpp; only the LVGL port lock has no other
// definition in the test build.
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

// ===========================================================================
// Browse-response path: a browser created over a REAL CommInterfaceImpl and
// StatisticsManager, fed real wire payloads through the same listener slot
// production uses. This exercises file_browser.cpp's response parsing,
// pagination, ".." sorting, and truncation handling - the logic the
// navigation tests above cannot reach.
// ===========================================================================

using WaveX::Protocol::BrowseRespHeader;
using WaveX::Protocol::FileEntryWire;
using WaveX::Test::GetInterMcuCapture;
using WaveX::Test::ResetInterMcuCapture;

namespace {

std::vector<uint8_t> BuildBrowsePayload(uint32_t total_count,
                                        const std::vector<FileEntryWire>& entries) {
    BrowseRespHeader header(total_count, static_cast<uint8_t>(entries.size()));
    std::vector<uint8_t> payload(sizeof(header) + entries.size() * sizeof(FileEntryWire));
    memcpy(payload.data(), &header, sizeof(header));
    if (!entries.empty()) {
        memcpy(payload.data() + sizeof(header),
               entries.data(),
               entries.size() * sizeof(FileEntryWire));
    }
    return payload;
}

}  // namespace

class FileBrowserResponseTest : public ::testing::Test {
   protected:
    void SetUp() override {
        ResetInterMcuCapture();
        stats_ = new StatisticsManager();
        comm_ = new WaveX::Comm::CommInterfaceImpl(*stats_);

        config_ = wavex_file_browser_config_t{};
        config_.root_path = "/";
        config_.file_extension = ".wav";
        config_.max_entries = 50;
        config_.show_hidden = false;
        config_.comm_interface = comm_;

        browser_ = wavex_file_browser_create(reinterpret_cast<lv_obj_t*>(&parent_), &config_);
        ASSERT_NE(browser_, nullptr);
    }

    void TearDown() override {
        if (browser_) {
            wavex_file_browser_destroy(browser_);
        }
        delete comm_;
        comm_ = nullptr;
        delete stats_;
        stats_ = nullptr;
    }

    // Delivers a browse response exactly the way the UART RX task does: via
    // the statistics manager's listener slot.
    void Respond(uint32_t total_count, const std::vector<FileEntryWire>& entries) {
        std::vector<uint8_t> payload = BuildBrowsePayload(total_count, entries);
        stats_->invoke_browse_resp_callback(payload.data(), payload.size());
    }

    int parent_ = 0;  // opaque dummy for the LVGL mock
    StatisticsManager* stats_ = nullptr;
    WaveX::Comm::CommInterfaceImpl* comm_ = nullptr;
    wavex_file_browser_config_t config_{};
    wavex_file_browser_t* browser_ = nullptr;
};

TEST_F(FileBrowserResponseTest, CreateSendsInitialBrowseRequestForRoot) {
    const auto& cap = GetInterMcuCapture();
    EXPECT_EQ(cap.browse_req_calls, 1);
    EXPECT_STREQ(cap.browse_req_path, "/");
    EXPECT_EQ(cap.browse_req_start_index, 0);
    // Unknown storage state must present as mounted until told otherwise.
    EXPECT_TRUE(wavex_file_browser_is_storage_mounted(browser_));
}

TEST_F(FileBrowserResponseTest, SinglePageResponsePopulatesEntriesWithMetadata) {
    Respond(2,
            {FileEntryWire(1, 0, "DRUMS"), FileEntryWire(0, 88200, "kick.wav", 44100, 2, 16, 500)});

    ASSERT_EQ(wavex_file_browser_get_entry_count(browser_), 2u);

    const wavex_file_entry_t* dir = wavex_file_browser_get_entry(browser_, 0);
    ASSERT_NE(dir, nullptr);
    EXPECT_STREQ(dir->name, "DRUMS");
    EXPECT_TRUE(dir->is_directory);

    const wavex_file_entry_t* file = wavex_file_browser_get_entry(browser_, 1);
    ASSERT_NE(file, nullptr);
    EXPECT_STREQ(file->name, "kick.wav");
    EXPECT_FALSE(file->is_directory);
    EXPECT_EQ(file->size_bytes, 88200u);
    EXPECT_EQ(file->sample_rate, 44100u);
    EXPECT_EQ(file->channels, 2);
    EXPECT_EQ(file->bits_per_sample, 16);
    EXPECT_EQ(file->duration_ms, 500u);
}

// A leading slash in the wire name (the Daisy sometimes sends one) must be
// stripped, and paths under a subdirectory must join without double slashes.
TEST_F(FileBrowserResponseTest, SubdirectoryPathsAreJoinedCorrectly) {
    wavex_file_browser_navigate_to(browser_, "/SOUNDS");
    Respond(2, {FileEntryWire(0, 100, "/kick.wav"), FileEntryWire(0, 200, "snare.wav")});

    ASSERT_EQ(wavex_file_browser_get_entry_count(browser_), 2u);
    const wavex_file_entry_t* first = wavex_file_browser_get_entry(browser_, 0);
    ASSERT_NE(first, nullptr);
    EXPECT_STREQ(first->name, "kick.wav") << "leading slash was not stripped";
    EXPECT_STREQ(first->path, "/SOUNDS/kick.wav");
    const wavex_file_entry_t* second = wavex_file_browser_get_entry(browser_, 1);
    ASSERT_NE(second, nullptr);
    EXPECT_STREQ(second->path, "/SOUNDS/snare.wav");
}

// At root the join must produce "/name", not "//name" (the pre-fix behavior:
// the leading slash was stripped from the name, then "%s/%s" of "/" + name
// doubled it back).
TEST_F(FileBrowserResponseTest, RootPathsAreJoinedWithoutDoubleSlash) {
    Respond(2, {FileEntryWire(0, 100, "/kick.wav"), FileEntryWire(1, 0, "DRUMS")});

    ASSERT_EQ(wavex_file_browser_get_entry_count(browser_), 2u);
    const wavex_file_entry_t* file = wavex_file_browser_get_entry(browser_, 0);
    ASSERT_NE(file, nullptr);
    EXPECT_STREQ(file->path, "/kick.wav");
    const wavex_file_entry_t* dir = wavex_file_browser_get_entry(browser_, 1);
    ASSERT_NE(dir, nullptr);
    EXPECT_STREQ(dir->path, "/DRUMS");
}

// ".." must be hoisted to the top of the first page when not at root, so the
// user can always leave a directory without scrolling.
TEST_F(FileBrowserResponseTest, ParentDirEntryIsSortedFirstOutsideRoot) {
    wavex_file_browser_navigate_to(browser_, "/SOUNDS");
    Respond(3,
            {FileEntryWire(0, 100, "a.wav"),
             FileEntryWire(1, 0, ".."),
             FileEntryWire(0, 200, "b.wav")});

    ASSERT_EQ(wavex_file_browser_get_entry_count(browser_), 3u);
    EXPECT_STREQ(wavex_file_browser_get_entry(browser_, 0)->name, "..");
    EXPECT_STREQ(wavex_file_browser_get_entry(browser_, 1)->name, "a.wav");
    EXPECT_STREQ(wavex_file_browser_get_entry(browser_, 2)->name, "b.wav");
}

// More files than one page: the first page must display immediately AND
// trigger a follow-up request at the right start index; the second response
// must append, not replace.
TEST_F(FileBrowserResponseTest, PaginationRequestsNextPageAndAccumulates) {
    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.browse_req_calls, 1);  // from create

    std::vector<FileEntryWire> page0;
    for (int i = 0; i < 20; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "f%02d.wav", i);
        page0.push_back(FileEntryWire(0, 100 + i, name));
    }
    Respond(25, page0);

    // First page is visible immediately...
    EXPECT_EQ(wavex_file_browser_get_entry_count(browser_), 20u);
    // ...and the next page was requested where this one ended.
    EXPECT_EQ(cap.browse_req_calls, 2);
    EXPECT_EQ(cap.browse_req_start_index, 20);

    std::vector<FileEntryWire> page1;
    for (int i = 20; i < 25; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "f%02d.wav", i);
        page1.push_back(FileEntryWire(0, 100 + i, name));
    }
    Respond(25, page1);

    ASSERT_EQ(wavex_file_browser_get_entry_count(browser_), 25u);
    EXPECT_STREQ(wavex_file_browser_get_entry(browser_, 0)->name, "f00.wav");
    EXPECT_STREQ(wavex_file_browser_get_entry(browser_, 24)->name, "f24.wav");
    // Pagination is complete: no third request.
    EXPECT_EQ(cap.browse_req_calls, 2);
}

// An empty listing is authoritative and can arrive UNSOLICITED (SD ejected):
// it must clear the stale listing and reset the selection.
TEST_F(FileBrowserResponseTest, UnsolicitedEmptyResponseClearsStaleListing) {
    Respond(2, {FileEntryWire(0, 100, "a.wav"), FileEntryWire(0, 200, "b.wav")});
    ASSERT_EQ(wavex_file_browser_get_entry_count(browser_), 2u);
    wavex_file_browser_set_selection(browser_, 1);

    Respond(0, {});

    EXPECT_EQ(wavex_file_browser_get_entry_count(browser_), 0u);
    EXPECT_EQ(wavex_file_browser_get_selected_index(browser_), 0u);
    EXPECT_EQ(wavex_file_browser_get_selected(browser_), nullptr);
}

// Truncated / malformed payloads must be rejected without touching memory
// past the buffer; the browser presents an empty (error) listing.
TEST_F(FileBrowserResponseTest, MalformedPayloadsAreRejected) {
    Respond(1, {FileEntryWire(0, 100, "a.wav")});
    ASSERT_EQ(wavex_file_browser_get_entry_count(browser_), 1u);

    // Shorter than the header.
    uint8_t junk[3] = {0x01, 0x02, 0x03};
    stats_->invoke_browse_resp_callback(junk, sizeof(junk));
    EXPECT_EQ(wavex_file_browser_get_entry_count(browser_), 0u);

    // Header claims more entries than the payload carries. Refresh first so
    // the browser is in a clean listing cycle.
    wavex_file_browser_refresh(browser_);
    Respond(1, {FileEntryWire(0, 100, "a.wav")});
    ASSERT_EQ(wavex_file_browser_get_entry_count(browser_), 1u);
    std::vector<uint8_t> lying = BuildBrowsePayload(5, {FileEntryWire(0, 100, "a.wav")});
    BrowseRespHeader bad_header(5, 5);  // claims 5 entries, carries 1
    memcpy(lying.data(), &bad_header, sizeof(bad_header));
    stats_->invoke_browse_resp_callback(lying.data(), lying.size());
    EXPECT_EQ(wavex_file_browser_get_entry_count(browser_), 0u);
}

// A name at the wire-format maximum (47 chars + NUL) must survive intact,
// and a full path longer than the 96-byte path field must be truncated with
// termination, not overflowed.
TEST_F(FileBrowserResponseTest, NamesAtBufferLimitAreBoundedAndTerminated) {
    std::string long_name(43, 'n');
    long_name += ".wav";  // 47 chars: the longest name the wire can carry
    ASSERT_EQ(long_name.size(), 47u);

    std::string deep_path = "/";
    deep_path += std::string(70, 'd');  // 71-char directory path
    wavex_file_browser_navigate_to(browser_, deep_path.c_str());

    Respond(1, {FileEntryWire(0, 100, long_name.c_str())});

    ASSERT_EQ(wavex_file_browser_get_entry_count(browser_), 1u);
    const wavex_file_entry_t* entry = wavex_file_browser_get_entry(browser_, 0);
    ASSERT_NE(entry, nullptr);
    EXPECT_STREQ(entry->name, long_name.c_str());

    // 71 (dir) + 1 (slash) + 47 (name) = 119 > 95: must truncate in-bounds.
    EXPECT_EQ(strlen(entry->path), sizeof(entry->path) - 1);
    EXPECT_EQ(strncmp(entry->path, deep_path.c_str(), deep_path.size()), 0);
}

TEST_F(FileBrowserResponseTest, MaxEntriesCapIsRespected) {
    // Rebuild with a small cap.
    wavex_file_browser_destroy(browser_);
    config_.max_entries = 5;
    browser_ = wavex_file_browser_create(reinterpret_cast<lv_obj_t*>(&parent_), &config_);
    ASSERT_NE(browser_, nullptr);

    std::vector<FileEntryWire> page;
    for (int i = 0; i < 10; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "f%02d.wav", i);
        page.push_back(FileEntryWire(0, 100, name));
    }
    Respond(10, page);

    EXPECT_EQ(wavex_file_browser_get_entry_count(browser_), 5u);
    // The cap also ends pagination: no follow-up request for entries that
    // could never be stored.
    EXPECT_EQ(GetInterMcuCapture().browse_req_calls, 2);  // create + rebuild only
}

// SD-eject then re-insert: the mounted flag must track the notifications, and
// a re-insert must re-list the current directory automatically.
TEST_F(FileBrowserResponseTest, StorageStatusUpdatesFlagAndRelistsOnMount) {
    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.browse_req_calls, 1);

    stats_->invoke_storage_status_callback(false);
    EXPECT_FALSE(wavex_file_browser_is_storage_mounted(browser_));
    EXPECT_EQ(cap.browse_req_calls, 1) << "loss must not trigger a re-list";

    stats_->invoke_storage_status_callback(true);
    EXPECT_TRUE(wavex_file_browser_is_storage_mounted(browser_));
    EXPECT_EQ(cap.browse_req_calls, 2) << "re-insert must re-list";
    EXPECT_STREQ(cap.browse_req_path, "/");
}

// Destroy must deregister both listeners BEFORE freeing: a response arriving
// after destroy (reachable by pressing Back mid-pagination) must be a no-op,
// not a write through freed memory.
TEST_F(FileBrowserResponseTest, DestroyDeregistersListeners) {
    wavex_file_browser_destroy(browser_);
    browser_ = nullptr;

    Respond(1, {FileEntryWire(0, 100, "late.wav")});
    stats_->invoke_storage_status_callback(true);
    // Reaching here without touching freed memory is the point; ASan/valgrind
    // turns a regression into a hard failure.
    SUCCEED();
}
