#include "comm/log_ring.h"
#include "config/hardware_config.h"  // For WAVEX_INTER_MCU_LINK_ENABLED
#include "daisy_spi_link.h"

#if WAVEX_INTER_MCU_LINK_ENABLED

// Force platform define for linter
#ifndef DAISY_PLATFORM
#define DAISY_PLATFORM 1
#endif

#include <stdint.h>
#include <stdio.h>  // For FILE operations
#include <string.h>
#include <strings.h>  // For strcasecmp

#include "../audio/audio_engine.h"   // For sample audition
#include "../storage/fs_browse.h"    // For file browsing
#include "config/hardware_config.h"  // For WAVEX_INTER_MCU_LINK_ENABLED
#include "config/logging_config.h"   // For logging macros
#include "daisy_filesystem.h"
#include "daisy_inter_mcu_message_handlers.h"
#include "daisy_seed.h"
#include "ff.h"
#include "mcu_link.h"
#include "spi_protocol/protocol.h"  // For WaveX::Protocol namespace
#include "sys/dma.h"                // For DMA_BUFFER_MEM_SECTION

#include "browse_response_outbox.hpp"
#include "wav/wav_header_parser.hpp"
#include "wxi/tag_scan.hpp"

// Hardware instance (shared with UART link) - accessed via WaveX::Comm::s_hw

using namespace daisy;
using namespace WaveX::Protocol;

// FileSystem class declaration
namespace WaveX {
namespace Storage {
class FileSystem {
   public:
    static bool GetFilePathByIndex(uint32_t file_index, char* file_path, size_t max_len);
};
}  // namespace Storage
}  // namespace WaveX

// Directory state for file browsing
static char s_current_directory[96] = "/";
static WaveX::Storage::FileEntry s_current_file_entries[BROWSE_DIRECTORY_ENTRY_LIMIT];
static size_t s_current_file_count = 0;
static bool s_directory_state_valid = false;
static BrowseFilter s_directory_filter = BrowseFilter::All;
static struct {
    bool active = false, started = false;
    size_t cursor = 0, kept = 0, start = 0;
    uint8_t max_entries = 20, mask = 0;
    WaveX::Wxi::TagScan scan;
} s_tag_filter;
static WaveX::Comm::BrowseResponseOutbox s_browse_response;
static uint32_t s_browse_request_id = 0;
static uint8_t s_browse_response_type = WaveX::Protocol::MSG_BROWSE_RESP;
// Keep FIL off the stack and aligned; place in default BSS (cache managed by driver).
alignas(32) static FIL s_metadata_file;
// Metadata read buffer: 4KB, aligned, in normal BSS (non-DTCM) so cache maintenance works.
// 4KB matches the earlier probe size and is enough to find fmt+data in typical WAVs.
alignas(32) static uint8_t s_metadata_buf[4096];

// ============================================================================
// FileSystem Implementation
// ============================================================================

namespace WaveX {
namespace Storage {
bool FileSystem::GetFilePathByIndex(uint32_t file_index, char* file_path, size_t max_len) {
    if (!s_directory_state_valid || file_index >= s_current_file_count) {
        return false;  // No valid directory state or index out of range
    }

    const WaveX::Storage::FileEntry& entry = s_current_file_entries[file_index];

    size_t dir_len = strlen(s_current_directory);
    size_t name_len = strlen(entry.name);

    if (dir_len + 1 + name_len + 1 > max_len) {
        return false;  // Path too long
    }

    strcpy(file_path, s_current_directory);

    // Add "/" if current directory is not root and doesn't end with "/"
    if (strcmp(s_current_directory, "/") != 0 && s_current_directory[dir_len - 1] != '/') {
        strcat(file_path, "/");
    }

    strcat(file_path, entry.name);

    return true;
}
}  // namespace Storage
}  // namespace WaveX

// WAV file metadata parsing
// Returns true if fmt chunk was found; duration_ms_out (if provided) reports total parse time.
bool ParseWavMetadata(const WaveX::Storage::FileEntry& entry,
                      WaveX::Protocol::FileEntryWire& wire_entry,
                      uint32_t* duration_ms_out /*=nullptr*/) {
    using namespace WaveX::Protocol;

    // Initialize to zero in case of early return
    wire_entry.sample_rate = 0;
    wire_entry.channels = 0;
    wire_entry.bits_per_sample = 0;
    wire_entry.duration_ms = 0;

    if (duration_ms_out) {
        *duration_ms_out = 0;
    }

    char full_path[256];
    size_t dir_len = strlen(s_current_directory);
    size_t name_len = strlen(entry.name);

    if (dir_len + 1 + name_len + 1 > sizeof(full_path)) {
        // Path too long - skip metadata parsing
        return false;
    }

    strcpy(full_path, s_current_directory);
    if (strcmp(s_current_directory, "/") != 0 && s_current_directory[dir_len - 1] != '/') {
        strcat(full_path, "/");
    }
    strcat(full_path, entry.name);

    if (WaveX::Comm::s_hw) {
        WaveX::Log::PrintLine("WAV META PARSE START: %s", entry.name);
    }

    // FatFS paths require an explicit drive prefix (0:/) when using f_open
    char fs_path[sizeof(full_path) + 2];
    if (strncmp(full_path, "0:", 2) == 0) {
        strncpy(fs_path, full_path, sizeof(fs_path) - 1);
        fs_path[sizeof(fs_path) - 1] = '\0';
    } else {
        snprintf(fs_path, sizeof(fs_path), "0:%s", full_path);
    }

    uint32_t parse_start_ms = daisy::System::GetNow();
    uint32_t elapsed_ms = 0;
    constexpr uint32_t METADATA_TIMEOUT_MS = 2000;  // 2 second timeout for metadata parsing

    // Avoid concurrent FatFS/SDMMC access while audio playback is active.
    // If playback is running, skip metadata to prevent stalls; UI will see duration=0.
    if (WaveX::AudioEngine::IsWavPlaying()) {
        return false;
    }

    // Use static FIL allocated in AXI SRAM so SDMMC DMA can reach its sector buffer
    // CRITICAL: Do NOT memset() the FIL - it has internal buffer pointers managed by FatFS.
    FIL& file = s_metadata_file;
    uint32_t t_open_start = daisy::System::GetNow();
    FRESULT fr = f_open(&file, fs_path, FA_READ | FA_OPEN_EXISTING);
    uint32_t t_open = daisy::System::GetNow() - t_open_start;
    if (WaveX::Comm::s_hw) {
        WaveX::Log::PrintLine("WAV META OPEN: %s t=%lu ms", entry.name, (unsigned long)t_open);
    }
    if (fr != FR_OK) {
        if (WaveX::Comm::s_hw) {
            WaveX::Log::PrintLine(
                "WAV META OPEN FAIL: %s err=%d", entry.name, static_cast<int>(fr));
        }
        return false;
    }

    // Read the first chunk (4k bytes) – enough to cover RIFF+fmt in most WAVs
    static constexpr size_t kHeaderProbeSize = sizeof(s_metadata_buf);
    UINT bytes_read = 0;

    uint32_t t_read_start = daisy::System::GetNow();
    fr = f_read(&file, s_metadata_buf, kHeaderProbeSize, &bytes_read);
    uint32_t t_read = daisy::System::GetNow() - t_read_start;
    if (WaveX::Comm::s_hw) {
        WaveX::Log::PrintLine("WAV META READ: %s t=%lu ms bytes=%u",
                              entry.name,
                              (unsigned long)t_read,
                              (unsigned)bytes_read);
    }

    // Final timeout check happens after read
    elapsed_ms = daisy::System::GetNow() - parse_start_ms;
    if (elapsed_ms > METADATA_TIMEOUT_MS) {
        if (WaveX::Comm::s_hw) {
            WaveX::Log::PrintLine(
                "WAV META TIMEOUT total=%lu ms file=%s", (unsigned long)elapsed_ms, entry.name);
        }
        if (duration_ms_out)
            *duration_ms_out = elapsed_ms;
        f_close(&file);
        return false;
    }

    if (fr != FR_OK || bytes_read < 12) {
        if (WaveX::Comm::s_hw) {
            WaveX::Log::PrintLine("WAV META HEADER FAIL %s bytes=%u fr=%d",
                                  entry.name,
                                  (unsigned)bytes_read,
                                  (int)fr);
        }
        if (duration_ms_out)
            *duration_ms_out = elapsed_ms;
        f_close(&file);
        return false;
    }

    f_close(&file);

    // Shared RIFF walk over the probe (wav/wav_header_parser.hpp, review
    // M12). On a partial parse (e.g. the data chunk lies beyond the 4 KB
    // probe) the parser still yields whatever fmt fields it saw - exactly
    // what the old hand-rolled loop did.
    WaveX::Wav::WavInfo info;
    WaveX::Wav::MemReader reader(s_metadata_buf, bytes_read);
    const auto parse_result = WaveX::Wav::ParseWavHeader(reader, info);
    if (parse_result == WaveX::Wav::ParseResult::NotRiffWave) {
        if (WaveX::Comm::s_hw) {
            WaveX::Log::PrintLine("WAV META NOT RIFF/WAVE: %s", entry.name);
        }
        if (duration_ms_out)
            *duration_ms_out = elapsed_ms;
        return false;
    }
    const bool fmt_found = (info.sample_rate != 0 || info.num_channels != 0);
    const bool data_found = (parse_result == WaveX::Wav::ParseResult::Ok);
    const uint32_t sample_rate = info.sample_rate;
    const uint16_t channels = info.num_channels;
    const uint16_t bits_per_sample = info.bits_per_sample;

    if (!fmt_found) {
        if (WaveX::Comm::s_hw) {
            WaveX::Log::PrintLine("WAV META MISSING fmt: %s", entry.name);
        }
        if (duration_ms_out)
            *duration_ms_out = elapsed_ms;
        return false;
    }

    wire_entry.sample_rate = sample_rate;
    wire_entry.channels = channels;
    wire_entry.bits_per_sample = bits_per_sample;
    wire_entry.duration_ms = 0;

    if (data_found) {
        // Shared helper, not an inline multiply: the 32-bit form overflows
        // above 97 s at 44.1 kHz. See WaveX::Wav::DurationMs.
        wire_entry.duration_ms = WaveX::Wav::DurationMs(info);
    }

    uint32_t parse_total_ms = daisy::System::GetNow() - parse_start_ms;
    if (duration_ms_out)
        *duration_ms_out = parse_total_ms;
    if (WaveX::Comm::s_hw) {
        WaveX::Log::PrintLine("WAV META DONE: %s total=%lu ms sr=%lu ch=%u bits=%u",
                              entry.name,
                              (unsigned long)parse_total_ms,
                              (unsigned long)wire_entry.sample_rate,
                              wire_entry.channels,
                              wire_entry.bits_per_sample);
    }
    return true;
}

// ============================================================================
// Filesystem Operations
// ============================================================================

namespace WaveX {
namespace Comm {
void ReplyBrowsePage(size_t start_index, uint8_t max_entries);

void ProcessBrowseRequest(const char* path,
                          size_t start_index,
                          uint8_t max_entries,
                          Protocol::BrowseFilter filter,
                          uint32_t request_id) {
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    WAVEX_LOGD(STORAGE,
               "IN MSG BROWSE_REQ path=%s start_index=%u max_entries=%u",
               path,
               (uint32_t)start_index,
               max_entries);

    s_browse_request_id = request_id;
    if (start_index && s_directory_state_valid && s_directory_filter == filter &&
        std::strcmp(path, s_current_directory) == 0) {
        ReplyBrowsePage(start_index, max_entries);
        return;
    }
    // Cache the directory state for index-based lookups
    strncpy(s_current_directory, path, sizeof(s_current_directory) - 1);
    s_current_directory[sizeof(s_current_directory) - 1] = '\0';

    // A new listing cancels only its own metadata scan. Files are read-only.
    s_tag_filter = {};
    s_directory_state_valid = false;
    s_current_file_count = 0;
    s_directory_filter = filter;
    s_browse_response.Begin();
    size_t total = 0;
    if (!ListDir(path,
                 s_current_file_entries,
                 BROWSE_DIRECTORY_ENTRY_LIMIT,
                 total,
                 0,
                 s_current_file_count,
                 BrowseTagMask(filter) ? BrowseFilter::Instruments : filter))
        return;
    if (BrowseTagMask(filter)) {
        s_tag_filter.active = true;
        s_tag_filter.mask = BrowseTagMask(filter);
        s_tag_filter.start = start_index;
        s_tag_filter.max_entries = max_entries;
        return;
    }
    s_directory_state_valid = true;
    ReplyBrowsePage(start_index, max_entries);
}

void ReplyBrowsePage(size_t start_index, uint8_t max_entries) {
    static constexpr size_t kMaxBrowseEntries =
        (BrowseResponseOutbox::kCapacity - sizeof(BrowsePageHeader) - 5) / sizeof(FileEntryWire);
    const size_t available =
        start_index < s_current_file_count ? s_current_file_count - start_index : 0;
    const size_t entries_written =
        std::min(available, std::min(size_t(max_entries), kMaxBrowseEntries));
    const auto* entries = s_current_file_entries + std::min(start_index, s_current_file_count);
    const size_t total_count = s_current_file_count;
    auto* browse_payload = s_browse_response.Begin();
    // Convert FileEntry to FileEntryWire for transmission
    static FileEntryWire wire_entries[50];  // static: see staging note above
    for (size_t i = 0; i < entries_written && i < 50; i++) {
        wire_entries[i].is_dir = entries[i].is_dir;
        wire_entries[i].size_bytes = entries[i].size_bytes;
        strncpy(wire_entries[i].name, entries[i].name, sizeof(wire_entries[i].name) - 1);
        wire_entries[i].name[sizeof(wire_entries[i].name) - 1] = '\0';

        // Parse WAV metadata during browse (DMA-safe buffers now prevent stalls).
        wire_entries[i].sample_rate = 0;
        wire_entries[i].channels = 0;
        wire_entries[i].bits_per_sample = 0;
        wire_entries[i].duration_ms = 0;
        if (!entries[i].is_dir && Protocol::BrowseExtensionEquals(entries[i].name, ".wav")) {
            ParseWavMetadata(entries[i], wire_entries[i], nullptr);
        }
    }

    // Create browse response payload: total_count (4 bytes) + n_entries (1 byte) + entries
    size_t payload_size = 0;
    s_browse_response_type = MSG_BROWSE_RESP;
    if (s_browse_request_id) {
        BrowsePageHeader page;
        page.request_id = s_browse_request_id;
        page.start_index = static_cast<uint8_t>(start_index);
        page.filter = s_directory_filter;
        memcpy(browse_payload, &page, sizeof(page));
        payload_size = sizeof(page);
        s_browse_response_type = MSG_BROWSE_PAGE_RESP;
    }

    // Copy total_count
    uint32_t total_count_le = total_count;
    memcpy(browse_payload + payload_size, &total_count_le, sizeof(uint32_t));
    payload_size += sizeof(uint32_t);

    // Copy n_entries count. In range: both branches bound entries_written by
    // actual_max_entries <= kMaxBrowseEntries (31).
    browse_payload[payload_size++] = static_cast<uint8_t>(entries_written);

    // Copy entries array
    if (entries_written > 0) {
        size_t entries_size = entries_written * sizeof(FileEntryWire);
        memcpy(browse_payload + payload_size, wire_entries, entries_size);
        payload_size += entries_size;
    }

    // Send the response via UART
    WAVEX_LOG_DAISY(STORAGE,
                    "Sending browse response via UART: total=%u count=%u size=%u",
                    (uint32_t)total_count,
                    (uint32_t)entries_written,
                    (uint32_t)payload_size);

    // In range: payload_size <= kBrowsePayloadCapacity (2048) by construction.
    s_browse_response.Commit(payload_size);
}

void PumpBrowseResponse() {
    if (s_tag_filter.active && !WaveX::AudioEngine::IsWavPlaying() &&
        !WaveX::AudioEngine::StorageJobBusy()) {
        auto& job = s_tag_filter;
        if (job.cursor == s_current_file_count) {
            job.active = false;
            s_current_file_count = job.kept;
            s_directory_state_valid = true;
            ReplyBrowsePage(job.start, job.max_entries);
        } else {
            const auto& entry = s_current_file_entries[job.cursor];
            if (entry.is_dir) {
                s_current_file_entries[job.kept++] = entry;
                ++job.cursor;
            } else if (!Protocol::BrowseExtensionEquals(entry.name, ".wxi")) {
                ++job.cursor;  // SFZ has no WaveX tag metadata; available under All.
            } else {
                // No file handle survives a main-loop pass: format/unmount and
                // other storage owners cannot inherit an open metadata file.
                char path[Protocol::BROWSE_PATH_MAX];
                std::snprintf(
                    path,
                    sizeof(path),
                    "0:%s%s%s",
                    s_current_directory,
                    s_current_directory[std::strlen(s_current_directory) - 1] == '/' ? "" : "/",
                    entry.name);
                if (f_open(&s_metadata_file, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
                    ++job.cursor;
                    job.started = false;
                } else {
                    if (!job.started) {
                        job.scan.Begin(f_size(&s_metadata_file));
                        job.started = true;
                    }
                    if (!job.scan.Done()) {
                        UINT read = 0;
                        const auto bytes = job.scan.Size();
                        if (f_lseek(&s_metadata_file, job.scan.Offset()) != FR_OK ||
                            f_read(&s_metadata_file, s_metadata_buf, bytes, &read) != FR_OK ||
                            read != bytes)
                            job.scan.Fail();
                        else
                            job.scan.Accept(s_metadata_buf, read);
                    }
                    if (f_close(&s_metadata_file) != FR_OK)
                        job.scan.Fail();
                    if (job.scan.Done()) {
                        if (job.scan.Valid() && (job.scan.Tags() & job.mask))
                            s_current_file_entries[job.kept++] = entry;
                        ++job.cursor;
                        job.started = false;
                    }
                }
            }
        }
    }

    // Do not block, dispatch recursively, or inflate overflow counters while
    // the queue drains. Main calls this before background status producers.
    if (LinkTxIdle()) {
        s_browse_response.Pump([](const uint8_t* payload, uint16_t size) {
            return LinkSend(s_browse_response_type, payload, size);
        });
    }
}

// Tells the frontend that storage went away: leave audition mode, and empty
// the browser listing because nothing on the card is reachable any more.
// Sent on card ejection and when reads fail past recovery. Without this the
// Daisy falls silent while the ESP32 still shows "Playing" over a file list
// it can no longer open.
void NotifyStorageLost() {
    s_tag_filter = {};

    using namespace WaveX::Protocol;
    s_directory_state_valid = false;
    s_current_file_count = 0;

    // Same message a user-requested stop sends, so the browser's existing
    // stop handling takes it - no new frontend state to get wrong.
    SampleStopRespMessage stop_resp;
    stop_resp.success = 1;
    stop_resp.reserved[0] = 0;
    stop_resp.reserved[1] = 0;
    stop_resp.reserved[2] = 0;
    WaveX::Comm::LinkSend(MSG_SAMPLE_STOP_RESP, &stop_resp, sizeof(stop_resp));

    // An empty browse response: total_count 0, n 0. Same shape the browser
    // already parses, so it clears the list through its normal path.
    s_browse_request_id = 0;
    s_browse_response_type = MSG_BROWSE_RESP;
    auto* empty_browse = s_browse_response.Begin();
    constexpr size_t empty_size = sizeof(uint32_t) + sizeof(uint8_t);
    memset(empty_browse, 0, empty_size);
    s_browse_response.Commit(empty_size);
    PumpBrowseResponse();

    StorageStatusMessage status(0);
    WaveX::Comm::LinkSend(MSG_STORAGE_STATUS, &status, sizeof(status));

    WaveX::Log::PrintLine("DAISY: storage lost - told frontend to exit audition and clear list");
}

// Storage is back. The frontend cannot poll for this - it has no view of the
// card slot - so it has to be told, or the browser sits on an empty listing
// until the user leaves the page and comes back.
void NotifyStorageAvailable() {
    using namespace WaveX::Protocol;
    StorageStatusMessage status(1);
    WaveX::Comm::LinkSend(MSG_STORAGE_STATUS, &status, sizeof(status));
    WaveX::Log::PrintLine("DAISY: storage available - frontend can re-list");
}

void ProcessSamplePlayRequest(const char* file_path) {
    using namespace WaveX::Protocol;
    using namespace WaveX::AudioEngine;

    if (WaveX::Comm::s_hw) {
        WaveX::Log::PrintLine("DAISY: ProcessSamplePlayRequest called with path: '%s'", file_path);
    }

    // Send immediate ACK to keep SPI responsive while we start playback
    {
        AckMessage ack;
        ack.serial_id = 0;  // TODO: Get actual serial ID if needed
        WaveX::Comm::LinkSend(WaveX::Protocol::MSG_ACK, &ack, sizeof(ack));
    }

    // Stop any current playback first (may touch filesystem/audio state)
    CloseWav();

    // Start playback using the existing WAV playback system
    if (!OpenWav(file_path)) {
        if (WaveX::Comm::s_hw) {
            WaveX::Log::PrintLine("DAISY: Failed to open WAV file for playback: '%s'", file_path);
        }

        // Send error response
        ErrorMessage error;
        error.code = 1;  // File open error
        strncpy(error.msg, "Failed to open WAV file", sizeof(error.msg) - 1);
        error.msg[sizeof(error.msg) - 1] = '\0';

        WaveX::Comm::LinkSend(WaveX::Protocol::MSG_ERROR, &error, sizeof(error));
        return;
    }

    if (WaveX::Comm::s_hw) {
        WaveX::Log::PrintLine("DAISY: Sample playback started successfully for: '%s'", file_path);
    }
}

void ProcessSampleStopRequest(uint8_t slot) {
    using namespace WaveX::Protocol;
    using namespace WaveX::AudioEngine;

    if (WaveX::Comm::s_hw) {
        WaveX::Log::PrintLine("DAISY: ProcessSampleStopRequest called (track=%u)", (unsigned)slot);
    }

    // Stop current playback.
    CloseWav();
    // Debug: report playback state after attempting stop
    if (WaveX::Comm::s_hw) {
        bool wav_playing = WaveX::AudioEngine::IsWavPlaying();
        WaveX::Log::PrintLine("DAISY: After stop - IsWavPlaying=%d", wav_playing ? 1 : 0);
    }

    // Send sample stop response
    SampleStopRespMessage stop_resp;
    stop_resp.success = 1;  // Successfully stopped
    stop_resp.reserved[0] = 0;
    stop_resp.reserved[1] = 0;
    stop_resp.reserved[2] = 0;

    WaveX::Comm::LinkSend(WaveX::Protocol::MSG_SAMPLE_STOP_RESP, &stop_resp, sizeof(stop_resp));

    if (WaveX::Comm::s_hw) {
        WaveX::Log::PrintLine("DAISY: Sample stop response sent");
    }
}

void ProcessSamplePlayIndexRequest(uint32_t file_index) {
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    if (WaveX::Comm::s_hw) {
        WaveX::Log::PrintLine("DAISY: ProcessSamplePlayIndexRequest called with index: %lu",
                              (unsigned long)file_index);
    }

    // Get file path for index
    char file_path[200] = {0};
    if (FileSystem::GetFilePathByIndex(file_index, file_path, sizeof(file_path))) {
        if (WaveX::Comm::s_hw) {
            WaveX::Log::PrintLine("DAISY: Playing sample at path: '%s'", file_path);
        }

        // Call the regular play request function
        ProcessSamplePlayRequest(file_path);
    } else {
        if (WaveX::Comm::s_hw) {
            WaveX::Log::PrintLine("DAISY: Failed to get file path for index %lu",
                                  (unsigned long)file_index);
        }
    }
}

void ProcessSampleGetPathRequest(uint32_t file_index) {
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    if (WaveX::Comm::s_hw) {
        WaveX::Log::PrintLine("DAISY: ProcessSampleGetPathRequest called with index: %lu",
                              (unsigned long)file_index);
    }

    // Get file path for index
    char file_path[200] = {0};
    if (FileSystem::GetFilePathByIndex(file_index, file_path, sizeof(file_path))) {
        SamplePathResponseMessage response;
        response.index = file_index;
        strncpy(response.path, file_path, sizeof(response.path) - 1);
        response.path[sizeof(response.path) - 1] = '\0';

        WaveX::Comm::LinkSend(
            WaveX::Protocol::MSG_SAMPLE_GET_PATH_RESP, &response, sizeof(response));

        if (WaveX::Comm::s_hw) {
            WaveX::Log::PrintLine("DAISY: Sent file path response: index=%lu path='%s'",
                                  (unsigned long)file_index,
                                  file_path);
        }
    } else {
        if (WaveX::Comm::s_hw) {
            WaveX::Log::PrintLine("DAISY: Failed to get file path for index %lu",
                                  (unsigned long)file_index);
        }
    }
}

}  // namespace Comm
}  // namespace WaveX

#endif  // WAVEX_INTER_MCU_LINK_ENABLED
