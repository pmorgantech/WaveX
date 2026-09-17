#include "fatfs_mock.h"

#include <algorithm>
#include <cstring>

extern "C" {

FRESULT f_getfree(const char*, DWORD* clusters, FATFS** fs) {
    auto& mock = MockFatFS::Instance();
    if (mock.free_result != FR_OK)
        return mock.free_result;
    static FATFS volume;
    volume.csize = mock.cluster_sectors;
    *clusters = mock.free_clusters;
    *fs = &volume;
    return FR_OK;
}

FRESULT f_open(FIL* file, const char* path, uint8_t mode) {
    if (!file || !path ||
        (mode != FA_READ && mode != (FA_WRITE | FA_CREATE_NEW) &&
         mode != (FA_WRITE | FA_CREATE_ALWAYS)))
        return FR_INT_ERR;
    *file = FIL{};
    auto& fs = MockFatFS::Instance();
    if (mode & FA_WRITE) {
        if ((mode & FA_CREATE_NEW) && fs.GetFile(path))
            return FR_EXIST;
        fs.AddFile(path, {});
        file->writable = fs.MutableFile(path);
    }
    file->bytes = fs.GetFile(path);
    if (!file->bytes)
        return FR_NO_FILE;
    static FATFS volume;
    file->obj.fs = &volume;
    return FR_OK;
}

FRESULT f_close(FIL* file) {
    if (!file)
        return FR_INT_ERR;
    const auto result = file->writable ? MockFatFS::Instance().close_result
                                       : MockFatFS::Instance().read_close_result;
    *file = FIL{};
    return result;
}

FRESULT f_read(FIL* file, void* out, UINT requested, UINT* read) {
    if (!file || !file->bytes || !read || (!out && requested))
        return FR_INVALID_OBJECT;
    *read = 0;
    if (MockFatFS::Instance().read_result != FR_OK)
        return MockFatFS::Instance().read_result;
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

FRESULT f_write(FIL* file, const void* source, UINT bytes, UINT* written) {
    if (!file || !file->writable || !written)
        return FR_INVALID_OBJECT;
    auto& fs = MockFatFS::Instance();
    *written = fs.write_limit < 0 ? bytes : std::min(bytes, static_cast<UINT>(fs.write_limit));
    if (fs.write_limit >= 0)
        fs.write_limit -= static_cast<int>(*written);
    auto& data = *file->writable;
    data.resize(file->position + *written);
    if (*written)
        std::memcpy(data.data() + file->position, source, *written);
    file->position += *written;
    return FR_OK;
}
FRESULT f_mkdir(const char* path) {
    auto& fs = MockFatFS::Instance();
    if (fs.mkdir_result != FR_OK)
        return fs.mkdir_result;
    if (fs.GetDirectory(path))
        return FR_EXIST;
    fs.AddDirectory(path, {});
    return FR_OK;
}
FRESULT f_stat(const char* path, FILINFO* info) {
    auto* bytes = MockFatFS::Instance().GetFile(path);
    const bool directory = MockFatFS::Instance().GetDirectory(path) != nullptr;
    if (!bytes && !directory)
        return FR_NO_FILE;
    if (info) {
        *info = FILINFO{};
        info->fsize = bytes ? static_cast<uint32_t>(bytes->size()) : 0;
        info->fattrib = directory ? AM_DIR : 0;
    }
    return FR_OK;
}
FRESULT f_rename(const char* from, const char* to) {
    auto& fs = MockFatFS::Instance();
    if (fs.rename_result != FR_OK)
        return fs.rename_result;
    if (fs.GetFile(to))
        return FR_EXIST;
    auto* data = fs.GetFile(from);
    if (!data)
        return FR_NO_FILE;
    fs.AddFile(to, *data);
    fs.RemoveFile(from);
    return FR_OK;
}
FRESULT f_unlink(const char* path) {
    return MockFatFS::Instance().RemoveFile(path) ? FR_OK
                                                  : MockFatFS::Instance().RemoveDirectory(path);
}
}  // extern "C"
