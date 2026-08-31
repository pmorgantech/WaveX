#include "fs_browse.h"

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

    const char* last_dot = strrchr(name, '.');
    if (!last_dot)
        return false;

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
        entries_written = 0;
        return false;
    }

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
        entries_written = 0;
        return false;
    }

    // Check if we're at root directory (need to manually add ".." when not at root)
    bool is_root =
        (strcmp(path, "/") == 0 || strlen(path) == 0 || (path[0] == '/' && strlen(path) == 1));

    // Single pass: collect all valid entries first, then paginate.
    // Static, not stack (review H6): 256 x sizeof(FileEntry) is ~14 KB,
    // previously carved out of the shared main-loop stack per call.
    // ListDir is main-loop-only and non-reentrant. Directories with more
    // than 256 qualifying entries are silently truncated (pre-existing
    // limit; the roadmap's 500-entry browse target needs a redesign here).
    static FileEntry all_entries[256];
    size_t all_count = 0;

    // Manually insert ".." entry at the beginning if not at root
    // FatFS may not always return ".." entries reliably
    if (!is_root && all_count < 256) {
        FileEntry& e = all_entries[all_count++];
        e.is_dir = 1;
        e.size_bytes = 0u;
        std::strncpy(e.name, "..", sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = '\0';
    }

    bool read_error = false;

    for (;;) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK) {
            // A mid-directory read error is NOT end-of-directory: returning
            // true here would hand the caller a silently truncated listing
            // presented as complete.
            read_error = true;
            break;
        }
        if (!fno.fname[0])
            break;
#if FF_USE_LFN
        const char* name = (fno.lfname && fno.lfname[0]) ? fno.lfname : fno.fname;
#else
        const char* name = fno.fname;
#endif
        if (is_dot_entry(name))
            continue;

        // Always skip a filesystem-returned "..": non-root listings get one
        // inserted manually above, and at root there is no parent to
        // navigate to (real FAT never returns dot entries at root, but the
        // contract shouldn't depend on that).
        if (strcmp(name, "..") == 0) {
            continue;
        }

        bool is_dir = (fno.fattrib & AM_DIR) ? true : false;
        if (is_dir || has_wav_extension(name)) {
            if (all_count < 256) {  // Prevent buffer overflow
                FileEntry& e = all_entries[all_count++];
                e.is_dir = is_dir ? 1 : 0;
                e.size_bytes = e.is_dir ? 0u : (uint32_t)fno.fsize;
                std::strncpy(e.name, name, sizeof(e.name) - 1);
                e.name[sizeof(e.name) - 1] = '\0';
            }
        }

        // all_entries is full: further f_readdir() calls would only be
        // discarded, but each one still does real SD I/O on the main loop
        // (shared with the WAV streaming ring buffer). Stop scanning here
        // instead of walking the rest of a large directory.
        if (all_count >= 256)
            break;
    }
    f_closedir(&dir);

    if (read_error) {
        total_count = 0;
        entries_written = 0;
        return false;
    }

    total_count = all_count;

    // Paginate. Special case: when start_index == 0 and a ".." entry exists,
    // it must land first regardless of pagination math below.
    size_t written = 0;
    bool has_dotdot_at_start =
        (!is_root && all_count > 0 && strcmp(all_entries[0].name, "..") == 0);

    if (start_index == 0 && has_dotdot_at_start && written < max_entries) {
        out[written++] = all_entries[0];
        start_index = 1;  // skip the ".." entry in the loop below
    }

    for (size_t i = start_index; i < all_count && written < max_entries; i++) {
        out[written++] = all_entries[i];
    }

    entries_written = written;
    return true;
}

}  // namespace Storage
}  // namespace WaveX
