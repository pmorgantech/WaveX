#include "fs_browse.h"

#include <stdio.h>    // For printf
#include <strings.h>  // for strcasecmp

#include "ff.h"

#include <cstring>

namespace WaveX {
namespace Storage {

static bool is_dot_entry(const char* name) {
    // Only filter "." entries, allow ".." entries for navigation
    return (name[0] == '.' && name[1] == '\0');
}

static bool has_wav_extension(const char* name) {
    if (!name)
        return false;

    // Find the last dot in the filename
    const char* last_dot = strrchr(name, '.');
    if (!last_dot)
        return false;

    // Check if it ends with .wav (case insensitive)
    const char* ext = last_dot + 1;
    return (strcasecmp(ext, "wav") == 0);
}

bool ListDir(const char* path,
             FileEntry* out,
             size_t max_entries,
             size_t& total_count,
             size_t start_index,
             size_t& entries_written) {
    if (!path || !out || max_entries == 0) {
        total_count = 0;
        return false;
    }

    printf("DAISY ListDir: path='%s', start_index=%zu, max_entries=%zu\n",
           path,
           start_index,
           max_entries);

    DIR dir;
    FILINFO fno;
#if FF_USE_LFN
    char lfn_buf[256];
    fno.lfname = lfn_buf;
    fno.lfsize = sizeof(lfn_buf);
#endif

    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        total_count = 0;
        return false;
    }

    // Check if we're at root directory (need to manually add ".." when not at root)
    bool is_root =
        (strcmp(path, "/") == 0 || strlen(path) == 0 || (path[0] == '/' && strlen(path) == 1));

    printf("DAISY ListDir: is_root=%d\n", is_root);

    // Single pass: collect all valid entries first, then paginate
    FileEntry all_entries[256];  // Buffer for all entries
    size_t all_count = 0;

    // Manually insert ".." entry at the beginning if not at root
    // FatFS may not always return ".." entries reliably
    if (!is_root && all_count < 256) {
        FileEntry& e = all_entries[all_count++];
        e.is_dir = 1;
        e.size_bytes = 0u;
        std::strncpy(e.name, "..", sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = '\0';
        printf("DAISY ListDir: Added '..' entry at index 0\n");
    }

    bool found_dotdot = !is_root;  // Track if we already added ".." manually

    for (;;) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || !fno.fname[0])
            break;
#if FF_USE_LFN
        const char* name = (fno.lfname && fno.lfname[0]) ? fno.lfname : fno.fname;
#else
        const char* name = fno.fname;
#endif
        if (is_dot_entry(name))
            continue;

        // Skip ".." from filesystem if we already added it manually
        if (found_dotdot && strcmp(name, "..") == 0) {
            continue;
        }

        // Include directories, WAV files, and ".." entries
        bool is_dir = (fno.fattrib & AM_DIR) ? true : false;
        bool is_parent_dir = (strcmp(name, "..") == 0);
        if (is_dir || has_wav_extension(name) || is_parent_dir) {
            if (all_count < 256) {  // Prevent buffer overflow
                FileEntry& e = all_entries[all_count++];
                e.is_dir = (is_dir || is_parent_dir) ? 1 : 0;
                e.size_bytes = e.is_dir ? 0u : (uint32_t)fno.fsize;
                std::strncpy(e.name, name, sizeof(e.name) - 1);
                e.name[sizeof(e.name) - 1] = '\0';
            }
        }
    }
    f_closedir(&dir);

    // Set total count
    total_count = all_count;

    // Now paginate the results
    // Special handling: when start_index == 0 and we have ".." entry, ensure it's always included
    // first
    size_t written = 0;
    bool has_dotdot_at_start =
        (!is_root && all_count > 0 && strcmp(all_entries[0].name, "..") == 0);

    printf("DAISY ListDir pagination: all_count=%zu, has_dotdot_at_start=%d, start_index=%zu\n",
           all_count,
           has_dotdot_at_start,
           start_index);

    // If we're requesting from start (index 0) and ".." exists, ensure it's first
    if (start_index == 0 && has_dotdot_at_start && written < max_entries) {
        out[written++] = all_entries[0];
        printf("DAISY ListDir: Wrote '..' at position 0\n");
        // Adjust start_index to skip the ".." entry when iterating
        start_index = 1;
    }

    // Add remaining entries starting from adjusted start_index
    for (size_t i = start_index; i < all_count && written < max_entries; i++) {
        out[written++] = all_entries[i];
        if (written <= 3) {
            printf("DAISY ListDir: Wrote entry at position %zu: '%s'\n",
                   written - 1,
                   all_entries[i].name);
        }
    }

    printf("DAISY ListDir: Returning %zu entries (total=%zu)\n", written, total_count);

    entries_written = written;
    return true;
}

}  // namespace Storage
}  // namespace WaveX
