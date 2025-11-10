#include <gtest/gtest.h>

#include <cstring>

// Include mock header first to get MockFileEntry and MockFatFS
#include "../../mocks/fatfs_mock.h"
// Include fs_browse.h - it will use our mock ff.h from the mocks directory
#include "fs_browse.h"

using namespace WaveX::Storage;

class FsBrowseTest : public ::testing::Test {
   protected:
    void SetUp() override { MockFatFS::Instance().Reset(); }

    void TearDown() override { MockFatFS::Instance().Reset(); }

    // Helper to create test directory structure
    void CreateTestDirectory(const std::string& path, const std::vector<MockFileEntry>& entries) {
        MockFatFS::Instance().AddDirectory(path, entries);
    }
};

// Test: ListDir with empty directory
TEST_F(FsBrowseTest, EmptyDirectory) {
    CreateTestDirectory("/", {});

    FileEntry entries[10];
    size_t total_count = 0;
    size_t entries_written = 0;

    bool result = ListDir("/", entries, 10, total_count, 0, entries_written);

    EXPECT_TRUE(result);
    EXPECT_EQ(0, total_count);
    EXPECT_EQ(0, entries_written);
}

// Test: ListDir with root directory containing files and directories
TEST_F(FsBrowseTest, RootDirectoryWithEntries) {
    CreateTestDirectory(
        "/",
        {
            MockFileEntry("SOUNDS", true),  // Directory
            MockFileEntry("test1.wav", false, 1024),
            MockFileEntry("test2.wav", false, 2048),
            MockFileEntry("readme.txt", false, 512),  // Should be filtered out (not .wav)
        });

    FileEntry entries[10];
    size_t total_count = 0;
    size_t entries_written = 0;

    bool result = ListDir("/", entries, 10, total_count, 0, entries_written);

    EXPECT_TRUE(result);
    EXPECT_EQ(3, total_count);  // SOUNDS, test1.wav, test2.wav (readme.txt filtered)
    EXPECT_EQ(3, entries_written);

    // Verify entries
    EXPECT_EQ(1, entries[0].is_dir);
    EXPECT_STREQ("SOUNDS", entries[0].name);

    EXPECT_EQ(0, entries[1].is_dir);
    EXPECT_STREQ("test1.wav", entries[1].name);
    EXPECT_EQ(1024u, entries[1].size_bytes);

    EXPECT_EQ(0, entries[2].is_dir);
    EXPECT_STREQ("test2.wav", entries[2].name);
    EXPECT_EQ(2048u, entries[2].size_bytes);
}

// Test: ListDir with subdirectory (should include ".." entry)
TEST_F(FsBrowseTest, SubdirectoryWithParentEntry) {
    CreateTestDirectory("/SOUNDS",
                        {
                            MockFileEntry("sample1.wav", false, 4096),
                            MockFileEntry("sample2.wav", false, 8192),
                        });

    FileEntry entries[10];
    size_t total_count = 0;
    size_t entries_written = 0;

    bool result = ListDir("/SOUNDS", entries, 10, total_count, 0, entries_written);

    EXPECT_TRUE(result);
    EXPECT_EQ(3, total_count);  // "..", sample1.wav, sample2.wav
    EXPECT_EQ(3, entries_written);

    // First entry should be ".."
    EXPECT_EQ(1, entries[0].is_dir);
    EXPECT_STREQ("..", entries[0].name);

    // Then the files
    EXPECT_EQ(0, entries[1].is_dir);
    EXPECT_STREQ("sample1.wav", entries[1].name);

    EXPECT_EQ(0, entries[2].is_dir);
    EXPECT_STREQ("sample2.wav", entries[2].name);
}

// Test: ListDir pagination - first page
TEST_F(FsBrowseTest, PaginationFirstPage) {
    std::vector<MockFileEntry> entries;
    for (int i = 0; i < 20; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file%02d.wav", i);
        entries.push_back(MockFileEntry(name, false, 1000 + i));
    }
    CreateTestDirectory("/", entries);

    FileEntry out_entries[5];
    size_t total_count = 0;
    size_t entries_written = 0;

    bool result = ListDir("/", out_entries, 5, total_count, 0, entries_written);

    EXPECT_TRUE(result);
    EXPECT_EQ(20, total_count);
    EXPECT_EQ(5, entries_written);

    // Verify first 5 entries
    EXPECT_STREQ("file00.wav", out_entries[0].name);
    EXPECT_STREQ("file04.wav", out_entries[4].name);
}

// Test: ListDir pagination - middle page
TEST_F(FsBrowseTest, PaginationMiddlePage) {
    std::vector<MockFileEntry> entries;
    for (int i = 0; i < 20; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file%02d.wav", i);
        entries.push_back(MockFileEntry(name, false, 1000 + i));
    }
    CreateTestDirectory("/", entries);

    FileEntry out_entries[5];
    size_t total_count = 0;
    size_t entries_written = 0;

    bool result = ListDir("/", out_entries, 5, total_count, 5, entries_written);

    EXPECT_TRUE(result);
    EXPECT_EQ(20, total_count);
    EXPECT_EQ(5, entries_written);

    // Verify entries 5-9
    EXPECT_STREQ("file05.wav", out_entries[0].name);
    EXPECT_STREQ("file09.wav", out_entries[4].name);
}

// Test: ListDir pagination - last page (partial)
TEST_F(FsBrowseTest, PaginationLastPage) {
    std::vector<MockFileEntry> entries;
    for (int i = 0; i < 18; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file%02d.wav", i);
        entries.push_back(MockFileEntry(name, false, 1000 + i));
    }
    CreateTestDirectory("/", entries);

    FileEntry out_entries[10];
    size_t total_count = 0;
    size_t entries_written = 0;

    bool result = ListDir("/", out_entries, 10, total_count, 15, entries_written);

    EXPECT_TRUE(result);
    EXPECT_EQ(18, total_count);
    EXPECT_EQ(3, entries_written);  // Only 3 entries left (15, 16, 17)

    EXPECT_STREQ("file15.wav", out_entries[0].name);
    EXPECT_STREQ("file17.wav", out_entries[2].name);
}

// Test: ListDir with subdirectory pagination (ensures ".." is first)
TEST_F(FsBrowseTest, SubdirectoryPaginationWithParent) {
    std::vector<MockFileEntry> entries;
    for (int i = 0; i < 10; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file%02d.wav", i);
        entries.push_back(MockFileEntry(name, false, 1000 + i));
    }
    CreateTestDirectory("/SOUNDS", entries);

    FileEntry out_entries[5];
    size_t total_count = 0;
    size_t entries_written = 0;

    bool result = ListDir("/SOUNDS", out_entries, 5, total_count, 0, entries_written);

    EXPECT_TRUE(result);
    EXPECT_EQ(11, total_count);  // ".." + 10 files
    EXPECT_EQ(5, entries_written);

    // First entry must be ".."
    EXPECT_EQ(1, out_entries[0].is_dir);
    EXPECT_STREQ("..", out_entries[0].name);

    // Then files
    EXPECT_STREQ("file00.wav", out_entries[1].name);
}

// Test: ListDir filters out "." entries
TEST_F(FsBrowseTest, FiltersDotEntries) {
    CreateTestDirectory("/",
                        {
                            MockFileEntry(".", true),   // Should be filtered
                            MockFileEntry("..", true),  // Should be included (if not root)
                            MockFileEntry("test.wav", false, 1024),
                        });

    FileEntry entries[10];
    size_t total_count = 0;
    size_t entries_written = 0;

    bool result = ListDir("/", entries, 10, total_count, 0, entries_written);

    EXPECT_TRUE(result);
    // "." should be filtered, ".." should be included, test.wav included
    EXPECT_EQ(2, total_count);  // "..", test.wav (or just test.wav if root)
    EXPECT_EQ(2, entries_written);
}

// Test: ListDir with invalid path
TEST_F(FsBrowseTest, InvalidPath) {
    FileEntry entries[10];
    size_t total_count = 0;
    size_t entries_written = 0;

    bool result = ListDir("/nonexistent", entries, 10, total_count, 0, entries_written);

    EXPECT_FALSE(result);
    EXPECT_EQ(0, total_count);
    EXPECT_EQ(0, entries_written);
}

// Test: ListDir with null parameters
TEST_F(FsBrowseTest, NullParameters) {
    FileEntry entries[10];
    size_t total_count = 0;
    size_t entries_written = 0;

    // Null path
    bool result1 = ListDir(nullptr, entries, 10, total_count, 0, entries_written);
    EXPECT_FALSE(result1);

    // Null output array
    bool result2 = ListDir("/", nullptr, 10, total_count, 0, entries_written);
    EXPECT_FALSE(result2);

    // Zero max_entries
    bool result3 = ListDir("/", entries, 0, total_count, 0, entries_written);
    EXPECT_FALSE(result3);
}

// Test: ListDir with long filenames (LFN support)
TEST_F(FsBrowseTest, LongFilenames) {
    CreateTestDirectory(
        "/",
        {
            MockFileEntry("very_long_filename_that_exceeds_8_3_format.wav", false, 2048, true),
            MockFileEntry("short.wav", false, 1024),
        });

    FileEntry entries[10];
    size_t total_count = 0;
    size_t entries_written = 0;

    bool result = ListDir("/", entries, 10, total_count, 0, entries_written);

    EXPECT_TRUE(result);
    EXPECT_EQ(2, total_count);
    EXPECT_EQ(2, entries_written);

    // Verify long filename is handled
    EXPECT_STREQ("very_long_filename_that_exceeds_8_3_format.wav", entries[0].name);
    EXPECT_STREQ("short.wav", entries[1].name);
}

// Test: ListDir case-insensitive WAV extension
TEST_F(FsBrowseTest, CaseInsensitiveWavExtension) {
    CreateTestDirectory("/",
                        {
                            MockFileEntry("test.WAV", false, 1024),
                            MockFileEntry("test.WaV", false, 2048),
                            MockFileEntry("test.wav", false, 4096),
                            MockFileEntry("test.txt", false, 512),  // Should be filtered
                        });

    FileEntry entries[10];
    size_t total_count = 0;
    size_t entries_written = 0;

    bool result = ListDir("/", entries, 10, total_count, 0, entries_written);

    EXPECT_TRUE(result);
    EXPECT_EQ(3, total_count);  // All .wav variants, .txt filtered
    EXPECT_EQ(3, entries_written);
}
