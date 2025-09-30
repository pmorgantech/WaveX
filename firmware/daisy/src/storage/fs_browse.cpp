#include "fs_browse.h"
#include "ff.h"
#include <cstring>
#include <strings.h>  // for strcasecmp

namespace WaveX {
namespace Storage {

static bool is_dot_entry(const char* name)
{
    // Only filter "." entries, allow ".." entries for navigation
    return (name[0] == '.' && name[1] == '\0');
}

static bool has_wav_extension(const char* name)
{
    if (!name) return false;
    
    // Find the last dot in the filename
    const char* last_dot = strrchr(name, '.');
    if (!last_dot) return false;
    
    // Check if it ends with .wav (case insensitive)
    const char* ext = last_dot + 1;
    return (strcasecmp(ext, "wav") == 0);
}

bool ListDir(const char* path,
             FileEntry* out,
             size_t max_entries,
             size_t& total_count,
             size_t start_index,
             size_t& entries_written)
{
    if(!path || !out || max_entries == 0) { total_count = 0; return false; }

    DIR dir;
    FILINFO fno;
#if FF_USE_LFN
    char lfn_buf[256];
    fno.lfname = lfn_buf;
    fno.lfsize = sizeof(lfn_buf);
#endif

    FRESULT fr = f_opendir(&dir, path);
    if(fr != FR_OK) { 
        total_count = 0; 
        return false; 
    }

    // Single pass: collect all valid entries first, then paginate
    FileEntry all_entries[256]; // Buffer for all entries
    size_t all_count = 0;
    
    for(;;)
    {
        fr = f_readdir(&dir, &fno);
        if(fr != FR_OK || !fno.fname[0]) break;
#if FF_USE_LFN
        const char* name = (fno.lfname && fno.lfname[0]) ? fno.lfname : fno.fname;
#else
        const char* name = fno.fname;
#endif
        if(is_dot_entry(name)) continue;
        
        // Include directories, WAV files, and ".." entries
        bool is_dir = (fno.fattrib & AM_DIR) ? true : false;
        bool is_parent_dir = (strcmp(name, "..") == 0);
        if (is_dir || has_wav_extension(name) || is_parent_dir) {
            if (all_count < 256) { // Prevent buffer overflow
                FileEntry& e = all_entries[all_count++];
                e.is_dir     = (is_dir || is_parent_dir) ? 1 : 0;
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
    size_t written = 0;
    for (size_t i = start_index; i < all_count && written < max_entries; i++) {
        out[written++] = all_entries[i];
    }
    
    entries_written = written;
    return true;
}

} // namespace Storage
} // namespace WaveX


