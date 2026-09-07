#include "fatfs_mock.h"

#include <algorithm>
#include <cstring>

extern "C" {

FRESULT f_open(FIL* file, const char* path, uint8_t mode) {
    if (!file || !path || mode != FA_READ)
        return FR_INT_ERR;
    *file = FIL{};
    file->bytes = MockFatFS::Instance().GetFile(path);
    if (!file->bytes)
        return FR_NO_FILE;
    static FATFS volume;
    file->obj.fs = &volume;
    return FR_OK;
}

FRESULT f_close(FIL* file) {
    if (!file)
        return FR_INT_ERR;
    *file = FIL{};
    return FR_OK;
}

FRESULT f_read(FIL* file, void* out, UINT requested, UINT* read) {
    if (!file || !file->bytes || !read || (!out && requested))
        return FR_INVALID_OBJECT;
    *read = 0;
    if (file->error != FR_OK)
        return file->error;
    const size_t remaining = file->bytes->size() - file->position;
    *read = static_cast<UINT>(std::min<size_t>(requested, remaining));
    if (*read)
        std::memcpy(out, file->bytes->data() + file->position, *read);
    file->position += *read;
    return FR_OK;
}

FRESULT f_lseek(FIL* file, FSIZE_t position) {
    if (!file || !file->bytes)
        return FR_INVALID_OBJECT;
    file->position = std::min(position, f_size(file));
    return FR_OK;
}

char* f_gets(char* out, int capacity, FIL* file) {
    if (!out || capacity < 2 || !file || !file->bytes)
        return nullptr;
    int count = 0;
    while (count < capacity - 1) {
        UINT read = 0;
        char ch = 0;
        if (f_read(file, &ch, 1, &read) != FR_OK || read == 0)
            break;
        if (ch == '\r')
            continue;  // FatFs string mode strips CR.
        out[count++] = ch;
        if (ch == '\n')
            break;
    }
    out[count] = '\0';
    return count == 0 ? nullptr : out;
}

FRESULT f_opendir(DIR* dp, const char* path) {
    if (!dp || !path)
        return FR_INT_ERR;

    MockFatFS& fs = MockFatFS::Instance();

    // Injected hard failure (dead card etc.) takes precedence over lookup.
    if (fs.OpendirResult() != FR_OK)
        return fs.OpendirResult();

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

    // Injected mid-listing failure (SD error part-way through a directory).
    if (fs.ConsumeReaddirFailure())
        return fs.ReaddirFailResult();

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
