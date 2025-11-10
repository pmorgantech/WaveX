#include "fatfs_mock.h"

#include <cstring>

extern "C" {

FRESULT f_opendir(DIR* dp, const char* path) {
    if (!dp || !path)
        return FR_INT_ERR;

    MockFatFS& fs = MockFatFS::Instance();
    std::string path_str(path);

    // Normalize path
    if (path_str.empty() || path_str == ".") {
        path_str = "/";
    }

    // Check if directory exists
    const std::vector<MockFileEntry>* entries = fs.GetDirectory(path_str);
    if (!entries) {
        return FR_NO_PATH;
    }

    // Create handle and store it
    void* handle = fs.GetNextHandle();
    dp->handle = handle;
    fs.StoreHandle(handle, path_str);

    return FR_OK;
}

FRESULT f_readdir(DIR* dp, FILINFO* fno) {
    if (!dp || !fno)
        return FR_INT_ERR;

    MockFatFS& fs = MockFatFS::Instance();
    MockFileEntry entry;

    if (!fs.GetNextEntry(dp->handle, entry)) {
        // End of directory
        fno->fname[0] = '\0';
#if FF_USE_LFN
        if (fno->lfname) {
            fno->lfname[0] = '\0';
        }
#endif
        return FR_OK;  // FR_OK indicates end of directory
    }

    // Fill FILINFO structure
    fno->fattrib = entry.is_dir ? AM_DIR : AM_ARC;
    fno->fsize = entry.size;

    // Copy filename
    if (entry.is_lfn && fno->lfname && fno->lfsize > 0) {
        // Use long filename
        size_t copy_len =
            (entry.name.length() < fno->lfsize - 1) ? entry.name.length() : fno->lfsize - 1;
        std::strncpy(fno->lfname, entry.name.c_str(), copy_len);
        fno->lfname[copy_len] = '\0';
        // Set short name to empty or first 8 chars
        std::strncpy(fno->fname, entry.name.c_str(), 12);
        fno->fname[12] = '\0';
    } else {
        // Use short filename
        std::strncpy(fno->fname, entry.name.c_str(), 12);
        fno->fname[12] = '\0';
#if FF_USE_LFN
        if (fno->lfname) {
            fno->lfname[0] = '\0';
        }
#endif
    }

    return FR_OK;
}

FRESULT f_closedir(DIR* dp) {
    if (!dp)
        return FR_INT_ERR;

    MockFatFS& fs = MockFatFS::Instance();
    fs.RemoveHandle(dp->handle);
    dp->handle = nullptr;

    return FR_OK;
}

}  // extern "C"
