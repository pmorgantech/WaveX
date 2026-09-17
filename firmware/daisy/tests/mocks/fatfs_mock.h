#ifndef FATFS_MOCK_H
#define FATFS_MOCK_H

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Mock FatFS types and functions for unit testing
// This allows testing fs_browse.cpp without actual filesystem

// FatFS result codes
typedef enum {
    FR_OK = 0,
    FR_DISK_ERR,
    FR_INT_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_INVALID_NAME,
    FR_DENIED,
    FR_EXIST,
    FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE,
    FR_NOT_ENABLED,
    FR_NO_FILESYSTEM,
    FR_MKFS_ABORTED,
    FR_TIMEOUT,
    FR_LOCKED,
    FR_NOT_ENOUGH_CORE,
    FR_TOO_MANY_OPEN_FILES
} FRESULT;

// File attributes
#define AM_RDO 0x01  // Read only
#define AM_HID 0x02  // Hidden
#define AM_SYS 0x04  // System
#define AM_DIR 0x10  // Directory
#define AM_ARC 0x20  // Archive

// Enable LFN support for tests
#define FF_USE_LFN 1

// Mock DIR structure
typedef struct {
    void* handle;  // Internal handle for mock
} DIR;

// Mock FILINFO structure
typedef struct {
    uint32_t fsize;   // File size
    uint16_t fdate;   // Date
    uint16_t ftime;   // Time
    uint8_t fattrib;  // File attribute
    char fname[13];   // Short file name (8.3 format)
#if FF_USE_LFN
    char* lfname;     // Long file name pointer
    uint32_t lfsize;  // Size of lfname buffer
#endif
} FILINFO;

using UINT = unsigned int;
using FSIZE_t = uint32_t;
#define FA_READ 0x01
#define FA_CREATE_ALWAYS 0x08
using DWORD = uint32_t;
#define FA_WRITE 0x02
#define FA_CREATE_NEW 0x04

struct FATFS {
    uint32_t csize = 1;
};
struct FIL {
    struct {
        FATFS* fs = nullptr;
    } obj;
    const std::vector<uint8_t>* bytes = nullptr;
    std::vector<uint8_t>* writable = nullptr;
    FSIZE_t position = 0;
    FRESULT error = FR_OK;
};

#define f_size(fp) ((fp)->bytes ? static_cast<FSIZE_t>((fp)->bytes->size()) : 0u)
#define f_tell(fp) ((fp)->position)
#define f_eof(fp) (f_tell(fp) >= f_size(fp))
#define f_error(fp) ((fp)->error)

// Mock filesystem entry for testing
struct MockFileEntry {
    std::string name;
    bool is_dir;
    uint32_t size;
    bool is_lfn;  // Use long filename

    MockFileEntry() : name(""), is_dir(false), size(0), is_lfn(false) {}

    MockFileEntry(const std::string& n, bool dir = false, uint32_t s = 0, bool lfn = false)
        : name(n), is_dir(dir), size(s), is_lfn(lfn) {}
};

// Mock filesystem state
class MockFatFS {
   public:
    static MockFatFS& Instance() {
        static MockFatFS instance;
        return instance;
    }

    // Reset mock filesystem
    void Reset() {
        directories_.clear();
        files_.clear();
        current_dir_handles_.clear();
        dir_positions_.clear();
        next_handle_ = 1;
        write_limit = -1;
        close_result = FR_OK;
        read_close_result = read_result = FR_OK;
        rename_result = FR_OK;
        free_result = FR_OK;
        free_clusters = 1024 * 1024;
        cluster_sectors = 1;
        mkdir_result = FR_OK;
        opendir_result_ = FR_OK;
        readdir_successes_before_failure_ = -1;
        readdir_fail_result_ = FR_DISK_ERR;
    }

    int write_limit = -1;  // total bytes before a short write
    FRESULT close_result = FR_OK;
    FRESULT read_close_result = FR_OK, read_result = FR_OK;
    FRESULT rename_result = FR_OK;
    FRESULT free_result = FR_OK;
    uint32_t free_clusters = 1024 * 1024;
    uint32_t cluster_sectors = 1;
    FRESULT mkdir_result = FR_OK;
    std::vector<uint8_t>* MutableFile(const char* path) {
        auto it = files_.find(path);
        return it == files_.end() ? nullptr : &it->second;
    }
    bool RemoveFile(const char* path) { return files_.erase(path) != 0; }
    // --- Failure injection -------------------------------------------------
    // f_opendir(): force the next (and every subsequent) open to fail with
    // `r` until Reset(). Models a dead/removed card rather than a missing
    // directory (which AddDirectory-less paths already cover).
    void SetOpendirResult(FRESULT r) { opendir_result_ = r; }
    FRESULT OpendirResult() const { return opendir_result_; }

    // f_readdir(): the next `successes` calls behave normally, then every
    // call fails with `r` until Reset(). Models an SD error mid-listing.
    void FailReaddirAfter(int successes, FRESULT r = FR_DISK_ERR) {
        readdir_successes_before_failure_ = successes;
        readdir_fail_result_ = r;
    }
    bool ConsumeReaddirFailure() {
        if (readdir_successes_before_failure_ < 0)
            return false;  // injection not armed
        if (readdir_successes_before_failure_ == 0)
            return true;
        --readdir_successes_before_failure_;
        return false;
    }
    FRESULT ReaddirFailResult() const { return readdir_fail_result_; }

    void AddFile(const std::string& path, const std::vector<uint8_t>& data) { files_[path] = data; }
    const std::vector<uint8_t>* GetFile(const char* path) const {
        auto it = files_.find(path);
        return it == files_.end() ? nullptr : &it->second;
    }

    // Add a directory with entries
    void AddDirectory(const std::string& path, const std::vector<MockFileEntry>& entries) {
        directories_[path] = entries;
    }

    // Set current directory position for a DIR handle
    void SetDirPosition(void* handle, size_t pos) { dir_positions_[handle] = pos; }

    // Get directory entries
    const std::vector<MockFileEntry>* GetDirectory(const std::string& path) const {
        auto it = directories_.find(path);
        return (it != directories_.end()) ? &it->second : nullptr;
    }

    // Get next handle ID
    void* GetNextHandle() { return reinterpret_cast<void*>(next_handle_++); }

    // Store handle
    void StoreHandle(void* handle, const std::string& path) {
        current_dir_handles_[handle] = path;
        dir_positions_[handle] = 0;
    }

    // Get path for handle
    std::string GetPathForHandle(void* handle) const {
        auto it = current_dir_handles_.find(handle);
        return (it != current_dir_handles_.end()) ? it->second : "";
    }

    // Get next entry for handle
    bool GetNextEntry(void* handle, MockFileEntry& entry) {
        auto path_it = current_dir_handles_.find(handle);
        if (path_it == current_dir_handles_.end())
            return false;

        const std::string& path = path_it->second;
        auto dir_it = directories_.find(path);
        if (dir_it == directories_.end())
            return false;

        size_t& pos = dir_positions_[handle];
        if (pos >= dir_it->second.size())
            return false;

        entry = dir_it->second[pos++];
        return true;
    }

    // Increment position
    void IncrementPosition(void* handle) { dir_positions_[handle]++; }

    // Get current position
    size_t GetPosition(void* handle) const {
        auto it = dir_positions_.find(handle);
        return (it != dir_positions_.end()) ? it->second : 0;
    }

    // Remove handle
    void RemoveHandle(void* handle) {
        current_dir_handles_.erase(handle);
        dir_positions_.erase(handle);
    }

   private:
    std::map<std::string, std::vector<uint8_t>> files_;
    std::map<std::string, std::vector<MockFileEntry>> directories_;
    std::map<void*, std::string> current_dir_handles_;
    std::map<void*, size_t> dir_positions_;
    uintptr_t next_handle_ = 1;

    // Failure injection state (see accessors above).
    FRESULT opendir_result_ = FR_OK;
    int readdir_successes_before_failure_ = -1;
    FRESULT readdir_fail_result_ = FR_DISK_ERR;
};

// Mock FatFS functions
extern "C" {
FRESULT f_getfree(const char* path, DWORD* clusters, FATFS** fs);
FRESULT f_open(FIL* file, const char* path, uint8_t mode);
FRESULT f_close(FIL* file);
FRESULT f_write(FIL* file, const void* source, UINT bytes, UINT* written);
FRESULT f_mkdir(const char* path);
FRESULT f_stat(const char* path, FILINFO* info);
FRESULT f_rename(const char* from, const char* to);
FRESULT f_unlink(const char* path);
FRESULT f_read(FIL* file, void* out, UINT requested, UINT* read);
FRESULT f_lseek(FIL* file, FSIZE_t position);
char* f_gets(char* out, int capacity, FIL* file);
FRESULT f_opendir(DIR* dp, const char* path);
FRESULT f_readdir(DIR* dp, FILINFO* fno);
FRESULT f_closedir(DIR* dp);
}

#endif  // FATFS_MOCK_H
