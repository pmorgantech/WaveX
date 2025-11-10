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
        current_dir_handles_.clear();
        next_handle_ = 1;
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
    std::map<std::string, std::vector<MockFileEntry>> directories_;
    std::map<void*, std::string> current_dir_handles_;
    std::map<void*, size_t> dir_positions_;
    uintptr_t next_handle_ = 1;
};

// Mock FatFS functions
extern "C" {
FRESULT f_opendir(DIR* dp, const char* path);
FRESULT f_readdir(DIR* dp, FILINFO* fno);
FRESULT f_closedir(DIR* dp);
}

#endif  // FATFS_MOCK_H
