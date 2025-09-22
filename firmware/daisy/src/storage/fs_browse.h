#pragma once

#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace Storage {

struct FileEntry {
    uint8_t  is_dir;     // 1 if directory, 0 if file
    uint32_t size_bytes; // file size (0 for directories)
    char     name[48];   // base name (no path), UTF-8 truncated
};

// List directory entries starting at start_index (for pagination).
// - path: directory to list (e.g., "/" or "/SOUNDS").
// - out: array of FileEntry to fill (max elements = max_entries).
// - max_entries: capacity of out array.
// - total_count: returns total number of entries in directory (excluding . and ..).
// - start_index: first entry index to return.
// - entries_written: returns actual number of entries written to out array.
// Returns true on success.
bool ListDir(const char* path,
             FileEntry* out,
             size_t max_entries,
             size_t& total_count,
             size_t start_index,
             size_t& entries_written);

} // namespace Storage
} // namespace WaveX


