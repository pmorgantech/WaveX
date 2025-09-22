#include "fs_browse.h"
#include "ff.h"
#include <cstring>

namespace WaveX {
namespace Storage {

static bool is_dot_entry(const char* name)
{
    return (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')));
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
    if(fr != FR_OK) { total_count = 0; return false; }

    // First pass: count entries (excluding . and ..)
    total_count = 0;
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
        total_count++;
    }

    // Second pass: collect requested page
    f_closedir(&dir);
    fr = f_opendir(&dir, path);
    if(fr != FR_OK) { return false; }

    size_t skipped = 0;
    size_t written = 0;
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
        if(skipped < start_index) { skipped++; continue; }
        if(written >= max_entries) break;

        FileEntry& e = out[written++];
        e.is_dir     = (fno.fattrib & AM_DIR) ? 1 : 0;
        e.size_bytes = e.is_dir ? 0u : (uint32_t)fno.fsize;
        std::strncpy(e.name, name, sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = '\0';
    }
    f_closedir(&dir);
    // Return the actual number of entries written to the array
    entries_written = written;
    return true;
}

} // namespace Storage
} // namespace WaveX


