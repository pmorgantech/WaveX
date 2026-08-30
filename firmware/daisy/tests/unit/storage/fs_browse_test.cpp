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

// Test: ListDir filters out "." entries - verified by NAME, not count alone
// (a count of 2 can't distinguish "filtered '.'" from "filtered the file").
//
// NOTE (pins current production behavior, fs_browse.cpp): a filesystem-
// returned ".." at the ROOT is NOT filtered - is_root suppresses only the
// manual ".." insertion, and the parent-dir branch then admits the FS one.
// Real FatFS does not return ".." for the root of a FAT volume, so this is
// unreachable on hardware, but it is the function's actual contract today;
// if root-level ".." filtering is ever added, this test must change with it.
TEST_F(FsBrowseTest, FiltersDotEntriesByName) {
    CreateTestDirectory("/",
                        {
                            MockFileEntry(".", true),   // filtered
                            MockFileEntry("..", true),  // admitted (see note above)
                            MockFileEntry("test.wav", false, 1024),
                        });

    FileEntry entries[10];
    size_t total_count = 0;
    size_t entries_written = 0;

    bool result = ListDir("/", entries, 10, total_count, 0, entries_written);

    EXPECT_TRUE(result);
    ASSERT_EQ(2, entries_written);
    EXPECT_EQ(2, total_count);
    EXPECT_STREQ("..", entries[0].name);
    EXPECT_EQ(1, entries[0].is_dir);
    EXPECT_STREQ("test.wav", entries[1].name);
    EXPECT_EQ(0, entries[1].is_dir);
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

// Test: ListDir with null parameters. The outputs are seeded with sentinels
// so a rejected call that leaves them untouched is visible - pre-seeding
// them with the expected value would make the assertions vacuous.
TEST_F(FsBrowseTest, NullParameters) {
    CreateTestDirectory("/", {MockFileEntry("test.wav", false, 1024)});
    FileEntry entries[10];

    // Null path.
    size_t total_count = 999;
    size_t entries_written = 999;
    EXPECT_FALSE(ListDir(nullptr, entries, 10, total_count, 0, entries_written));
    EXPECT_EQ(0u, total_count) << "failure must zero total_count";
    // Pins current behavior: entries_written is NOT written on the guard
    // path - a caller may not read it unless ListDir returned true.
    EXPECT_EQ(999u, entries_written);

    // Null output array.
    total_count = 999;
    EXPECT_FALSE(ListDir("/", nullptr, 10, total_count, 0, entries_written));
    EXPECT_EQ(0u, total_count);

    // Zero max_entries.
    total_count = 999;
    EXPECT_FALSE(ListDir("/", entries, 0, total_count, 0, entries_written));
    EXPECT_EQ(0u, total_count);
}

// Test: an injected f_opendir hard failure (dead card, not merely a missing
// directory) fails the listing cleanly.
TEST_F(FsBrowseTest, OpendirDiskErrorReturnsFalse) {
    CreateTestDirectory("/", {MockFileEntry("test.wav", false, 1024)});
    MockFatFS::Instance().SetOpendirResult(FR_DISK_ERR);

    FileEntry entries[10];
    size_t total_count = 999;
    size_t entries_written = 999;
    EXPECT_FALSE(ListDir("/", entries, 10, total_count, 0, entries_written));
    EXPECT_EQ(0u, total_count);
}

// Test: an f_readdir error PART-WAY through the directory.
//
// NOTE (pins current production behavior, fs_browse.cpp read loop): a
// mid-listing FRESULT error is indistinguishable from end-of-directory -
// the loop breaks on `fr != FR_OK` and ListDir still returns TRUE with a
// silently truncated listing. The browser would show a partial directory
// with no error. If error propagation is ever added, flip the EXPECT_TRUE
// below deliberately.
TEST_F(FsBrowseTest, MidListingReaddirErrorTruncatesSilently) {
    CreateTestDirectory("/",
                        {
                            MockFileEntry("a.wav", false, 1),
                            MockFileEntry("b.wav", false, 2),
                            MockFileEntry("c.wav", false, 3),
                            MockFileEntry("d.wav", false, 4),
                        });
    MockFatFS::Instance().FailReaddirAfter(2, FR_DISK_ERR);

    FileEntry entries[10];
    size_t total_count = 0;
    size_t entries_written = 0;
    bool result = ListDir("/", entries, 10, total_count, 0, entries_written);

    EXPECT_TRUE(result);  // current contract: truncation, not failure
    ASSERT_EQ(2u, entries_written);
    EXPECT_EQ(2u, total_count);
    EXPECT_STREQ("a.wav", entries[0].name);
    EXPECT_STREQ("b.wav", entries[1].name);
}

// Test: the internal scratch array caps a listing at 256 entries (documented
// in fs_browse.cpp; the roadmap's 500-entry browse target needs a redesign).
// total_count must report the CAP, not the real directory size, and the call
// still succeeds.
TEST_F(FsBrowseTest, ListingIsCappedAt256Entries) {
    std::vector<MockFileEntry> big;
    for (int i = 0; i < 300; i++) {
        char name[32];
        snprintf(name, sizeof(name), "f%03d.wav", i);
        big.push_back(MockFileEntry(name, false, 100 + i));
    }
    CreateTestDirectory("/", big);

    FileEntry entries[8];
    size_t total_count = 0;
    size_t entries_written = 0;
    bool result = ListDir("/", entries, 8, total_count, 256 - 4, entries_written);

    EXPECT_TRUE(result);
    EXPECT_EQ(256u, total_count) << "entries beyond the 256-entry scratch array are dropped";
    // Only 4 entries remain past start_index 252, and they are the LAST ones
    // the scan admitted (f252..f255).
    ASSERT_EQ(4u, entries_written);
    EXPECT_STREQ("f252.wav", entries[0].name);
    EXPECT_STREQ("f255.wav", entries[3].name);
}

// Test: a name longer than FileEntry::name (48 bytes) is truncated to 47
// characters plus NUL - not overflowed, and not dropped.
TEST_F(FsBrowseTest, OverlongNameIsTruncatedTo47Chars) {
    // 58 characters - well past the 48-byte field. Extension filtering runs
    // on the FULL name (so it still qualifies as .wav); only the stored copy
    // is cut, at 47 characters.
    const char* long_name = "abcdefghij_abcdefghij_abcdefghij_abcdefghij_abcdefghij.wav";  // 58 ch
    CreateTestDirectory("/", {MockFileEntry(long_name, false, 2048, true)});

    FileEntry entries[4];
    size_t total_count = 0;
    size_t entries_written = 0;
    ASSERT_TRUE(ListDir("/", entries, 4, total_count, 0, entries_written));
    ASSERT_EQ(1u, entries_written);

    char expected[48];
    std::memcpy(expected, long_name, 47);
    expected[47] = '\0';
    EXPECT_STREQ(expected, entries[0].name);
    EXPECT_EQ(47u, std::strlen(entries[0].name));
}

// Test: start_index at/past the end of the listing writes nothing but is
// still a successful (empty) page, so a paging UI can probe past the end.
TEST_F(FsBrowseTest, StartIndexBeyondEndYieldsEmptySuccessfulPage) {
    CreateTestDirectory("/",
                        {
                            MockFileEntry("a.wav", false, 1),
                            MockFileEntry("b.wav", false, 2),
                        });

    FileEntry entries[4];
    size_t total_count = 0;
    size_t entries_written = 999;
    EXPECT_TRUE(ListDir("/", entries, 4, total_count, 10, entries_written));
    EXPECT_EQ(2u, total_count);
    EXPECT_EQ(0u, entries_written);
}

// Test: page 2 of a subdirectory listing continues where page 1 (which
// forced ".." first) left off - names checked so an off-by-one in the ".."
// special-casing shows up as a duplicated or skipped file.
TEST_F(FsBrowseTest, SubdirectorySecondPageContinuesWithoutGapOrOverlap) {
    std::vector<MockFileEntry> files;
    for (int i = 0; i < 9; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file%02d.wav", i);
        files.push_back(MockFileEntry(name, false, 1000 + i));
    }
    CreateTestDirectory("/SOUNDS", files);

    FileEntry page[5];
    size_t total_count = 0;
    size_t written = 0;

    // Page 1: "..", file00..file03.
    ASSERT_TRUE(ListDir("/SOUNDS", page, 5, total_count, 0, written));
    ASSERT_EQ(5u, written);
    EXPECT_EQ(10u, total_count);  // ".." + 9 files
    EXPECT_STREQ("..", page[0].name);
    EXPECT_STREQ("file00.wav", page[1].name);
    EXPECT_STREQ("file03.wav", page[4].name);

    // Page 2 (start_index 5): file04..file08, no repeat of file03, no skip.
    ASSERT_TRUE(ListDir("/SOUNDS", page, 5, total_count, 5, written));
    ASSERT_EQ(5u, written);
    EXPECT_STREQ("file04.wav", page[0].name);
    EXPECT_STREQ("file08.wav", page[4].name);
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
