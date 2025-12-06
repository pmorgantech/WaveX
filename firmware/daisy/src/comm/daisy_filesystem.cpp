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
#include "daisy_uart_link.h"
#include "ff.h"
#include "spi_protocol/protocol.h"  // For WaveX::Protocol namespace
#include "sys/dma.h"                // For DMA_BUFFER_MEM_SECTION

// Hardware instance (shared with UART link) - accessed via WaveX::Comm::s_hw

using namespace daisy;
using namespace WaveX::Protocol;

// Forward declaration for Spi_SendPreCreatedPacket (defined in main file)
extern bool Spi_SendPreCreatedPacket(const uint8_t* packet_data, size_t packet_size);

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
static WaveX::Storage::FileEntry s_current_file_entries[50];  // Increased to accommodate more files
static size_t s_current_file_count = 0;
static bool s_directory_state_valid = false;
// Keep FIL off the stack and aligned; place in default BSS (cache managed by driver).
alignas(32) static FIL s_metadata_file;
// Metadata read buffer: 4KB, aligned, in normal BSS (non-DTCM) so cache maintenance works.
// 4KB matches the earlier probe size and is enough to find fmt+data in typical WAVs.
alignas(32) static uint8_t s_metadata_buf[4096];

// ============================================================================
// FileSystem Implementation
// ============================================================================

// Implementation of FileSystem::GetFilePathByIndex
namespace WaveX {
namespace Storage {
bool FileSystem::GetFilePathByIndex(uint32_t file_index, char* file_path, size_t max_len) {
    // Use cached directory state to get file path by index
    if (!s_directory_state_valid || file_index >= s_current_file_count) {
        return false;  // No valid directory state or index out of range
    }

    const WaveX::Storage::FileEntry& entry = s_current_file_entries[file_index];

    // Build full path: current_directory + "/" + filename
    size_t dir_len = strlen(s_current_directory);
    size_t name_len = strlen(entry.name);

    // Check if we have enough space
    if (dir_len + 1 + name_len + 1 > max_len) {
        return false;  // Path too long
    }

    // Build the path
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

static inline uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static inline uint32_t read_le32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0])) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

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

    // Build full file path
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
        WaveX::Comm::s_hw->PrintLine("WAV META PARSE START: %s", entry.name);
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
        WaveX::Comm::s_hw->PrintLine("WAV META OPEN: %s t=%lu ms", entry.name, (unsigned long)t_open);
    }
    if (fr != FR_OK) {
        if (WaveX::Comm::s_hw) {
            WaveX::Comm::s_hw->PrintLine("WAV META OPEN FAIL: %s err=%d", entry.name, static_cast<int>(fr));
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
        WaveX::Comm::s_hw->PrintLine("WAV META READ: %s t=%lu ms bytes=%u",
                                     entry.name,
                                     (unsigned long)t_read,
                                     (unsigned)bytes_read);
    }

    // Final timeout check happens after read
    elapsed_ms = daisy::System::GetNow() - parse_start_ms;
    if (elapsed_ms > METADATA_TIMEOUT_MS) {
        if (WaveX::Comm::s_hw) {
            WaveX::Comm::s_hw->PrintLine("WAV META TIMEOUT total=%lu ms file=%s",
                                         (unsigned long)elapsed_ms,
                                         entry.name);
        }
        if (duration_ms_out) *duration_ms_out = elapsed_ms;
        f_close(&file);
        return false;
    }

    if (fr != FR_OK || bytes_read < 12) {
        if (WaveX::Comm::s_hw) {
            WaveX::Comm::s_hw->PrintLine("WAV META HEADER FAIL %s bytes=%u fr=%d",
                                         entry.name,
                                         (unsigned)bytes_read,
                                         (int)fr);
        }
        if (duration_ms_out) *duration_ms_out = elapsed_ms;
        f_close(&file);
        return false;
    }

    f_close(&file);

    if (memcmp(s_metadata_buf, "RIFF", 4) != 0 || memcmp(s_metadata_buf + 8, "WAVE", 4) != 0) {
        if (WaveX::Comm::s_hw) {
            WaveX::Comm::s_hw->PrintLine("WAV META NOT RIFF/WAVE: %s", entry.name);
        }
        if (duration_ms_out) *duration_ms_out = elapsed_ms;
        return false;
    }

    bool fmt_found = false;
    bool data_found = false;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_chunk_size = 0;

    size_t offset = 12;  // Skip RIFF header
    while (offset + 8 <= bytes_read) {
        const uint8_t* chunk = s_metadata_buf + offset;
        uint32_t chunk_size = read_le32(chunk + 4);
        char chunk_id[5] = {static_cast<char>(chunk[0]),
                            static_cast<char>(chunk[1]),
                            static_cast<char>(chunk[2]),
                            static_cast<char>(chunk[3]),
                            0};
        offset += 8;

        size_t bytes_available = (bytes_read > offset) ? (bytes_read - offset) : 0;
        if (chunk_size > bytes_available) {
            // Not enough data in our probe to parse this chunk – stop scanning
            break;
        }

        if (!fmt_found && memcmp(chunk_id, "fmt ", 4) == 0 && chunk_size >= 16) {
            const uint8_t* fmt = s_metadata_buf + offset;
            channels = read_le16(fmt + 2);
            sample_rate = read_le32(fmt + 4);
            bits_per_sample = read_le16(fmt + 14);
            fmt_found = true;
        } else if (!data_found && memcmp(chunk_id, "data", 4) == 0) {
            data_chunk_size = chunk_size;
            data_found = true;
        }

        offset += chunk_size;
        if (chunk_size & 1) {
            offset += 1;  // Pad byte alignment
        }

        if (fmt_found && data_found) {
            break;
        }
    }

    if (!fmt_found) {
        if (WaveX::Comm::s_hw) {
            WaveX::Comm::s_hw->PrintLine("WAV META MISSING fmt: %s", entry.name);
        }
        if (duration_ms_out) *duration_ms_out = elapsed_ms;
        return false;
    }

    wire_entry.sample_rate = sample_rate;
    wire_entry.channels = channels;
    wire_entry.bits_per_sample = bits_per_sample;
    wire_entry.duration_ms = 0;

    if (data_found && sample_rate > 0 && channels > 0 && bits_per_sample > 0) {
        uint32_t bytes_per_sample = bits_per_sample / 8;
        if (bytes_per_sample > 0) {
            uint32_t bytes_per_frame = bytes_per_sample * channels;
            if (bytes_per_frame > 0) {
                uint32_t frames = data_chunk_size / bytes_per_frame;
                wire_entry.duration_ms = (frames * 1000u) / sample_rate;
            }
        }
    }

    uint32_t parse_total_ms = daisy::System::GetNow() - parse_start_ms;
    if (duration_ms_out) *duration_ms_out = parse_total_ms;
    if (WaveX::Comm::s_hw) {
        WaveX::Comm::s_hw->PrintLine("WAV META DONE: %s total=%lu ms sr=%lu ch=%u bits=%u",
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

// Process browse request (existing function - updated for new format)
void ProcessBrowseRequest(const char* path, size_t start_index, uint8_t max_entries) {
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE,
                            "IN MSG BROWSE_REQ path=%s start_index=%u max_entries=%u",
                            path,
                            (uint32_t)start_index,
                            max_entries);

    // Cache the directory state for index-based lookups
    strncpy(s_current_directory, path, sizeof(s_current_directory) - 1);
    s_current_directory[sizeof(s_current_directory) - 1] = '\0';

    // Allocate buffer for file entries
    FileEntry entries[50];  // Max 50 entries per response (matches cache size)
    size_t actual_max_entries = (max_entries > 50) ? 50 : max_entries;

    size_t total_count = 0;
    size_t entries_written = 0;

    // Get directory listing from FatFS
    // OPTIMIZATION: If this is the first page (start_index == 0), get all entries first for
    // caching, then extract the paginated subset. This avoids calling ListDir twice.
    FileEntry all_entries[50];  // Buffer for all entries when caching
    size_t all_entries_count = 0;

    if (start_index == 0) {
        // Get all entries for caching (max 50)
        uint32_t listdir_start_ms = daisy::System::GetNow();
        bool success = ListDir(path, all_entries, 50, total_count, 0, all_entries_count);
        uint32_t listdir_end_ms = daisy::System::GetNow();
        uint32_t listdir_duration_ms = listdir_end_ms - listdir_start_ms;

        if (WaveX::Comm::s_hw) {
            WaveX::Comm::s_hw->PrintLine("DAISY: ListDir completed: t=%lu ms, duration=%lu ms",
                                         (unsigned long)listdir_end_ms,
                                         (unsigned long)listdir_duration_ms);
        }

        if (!success) {
            WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "Failed to list directory: %s", path);
            return;
        }

        // Cache all entries (up to cache limit)
        s_current_file_count = (all_entries_count > 50) ? 50 : all_entries_count;
        for (size_t i = 0; i < s_current_file_count; i++) {
            s_current_file_entries[i] = all_entries[i];
        }
        s_directory_state_valid = true;

        // Extract paginated subset for response
        entries_written = 0;
        size_t end_index =
            (all_entries_count < actual_max_entries) ? all_entries_count : actual_max_entries;
        for (size_t i = 0; i < end_index; i++) {
            entries[entries_written++] = all_entries[i];
        }

        if (WaveX::Comm::s_hw) {
            WaveX::Comm::s_hw->PrintLine(
                "DAISY: Cached directory state: %u entries from '%s' (sending %u)",
                (uint32_t)s_current_file_count,
                path,
                (uint32_t)entries_written);
        }
    } else {
        // For subsequent pages, just get the paginated entries (no caching needed)
        bool success =
            ListDir(path, entries, actual_max_entries, total_count, start_index, entries_written);

        if (!success) {
            WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "Failed to list directory: %s", path);
            return;
        }
    }

    WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE,
                            "Directory listing: total=%u written=%u",
                            (uint32_t)total_count,
                            (uint32_t)entries_written);

    // Convert FileEntry to FileEntryWire for transmission
    FileEntryWire wire_entries[50];
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
        if (!entries[i].is_dir) {
            ParseWavMetadata(entries[i], wire_entries[i], nullptr);
        }
    }

    // Create browse response payload: total_count (4 bytes) + n_entries (1 byte) + entries
    uint8_t browse_payload[2048];
    size_t payload_size = 0;

    // Copy total_count
    uint32_t total_count_le = total_count;
    memcpy(browse_payload, &total_count_le, sizeof(uint32_t));
    payload_size += sizeof(uint32_t);

    // Copy n_entries count
    browse_payload[payload_size++] = entries_written;

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

    int send_result = UartLinkSend(WaveX::Protocol::MSG_BROWSE_RESP, browse_payload, payload_size);
    if (send_result < 0) {
        WAVEX_LOG_DAISY(STORAGE, "Failed to send browse response (queue full?)");
    }
}

// Process sample play request (existing function)
void ProcessSamplePlayRequest(const char* file_path) {
    using namespace WaveX::Protocol;
    using namespace WaveX::AudioEngine;

    if (WaveX::Comm::s_hw) {
        WaveX::Comm::s_hw->PrintLine("DAISY: ProcessSamplePlayRequest called with path: '%s'",
                                     file_path);
    }

    // Send immediate ACK to keep SPI responsive while we start playback
    {
        AckMessage ack;
        ack.serial_id = 0;  // TODO: Get actual serial ID if needed
        WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_ACK, &ack, sizeof(ack));
    }

    // Stop any current playback first (may touch filesystem/audio state)
    CloseWav();

    // Start playback using the existing WAV playback system
    if (!OpenWav(file_path)) {
        if (WaveX::Comm::s_hw) {
            WaveX::Comm::s_hw->PrintLine("DAISY: Failed to open WAV file for playback: '%s'",
                                         file_path);
        }

        // Send error response
        ErrorMessage error;
        error.code = 1;  // File open error
        strncpy(error.msg, "Failed to open WAV file", sizeof(error.msg) - 1);
        error.msg[sizeof(error.msg) - 1] = '\0';

        WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_ERROR, &error, sizeof(error));
        return;
    }

    if (WaveX::Comm::s_hw) {
        WaveX::Comm::s_hw->PrintLine("DAISY: Sample playback started successfully for: '%s'",
                                     file_path);
    }
}

// Process sample stop request (existing function)
void ProcessSampleStopRequest(uint8_t slot) {
    using namespace WaveX::Protocol;
    using namespace WaveX::AudioEngine;

    if (WaveX::Comm::s_hw) {
        WaveX::Comm::s_hw->PrintLine("DAISY: ProcessSampleStopRequest called (slot=%u)",
                                     (unsigned)slot);
    }

    // Stop current playback. Call CloseWav() to stop any WAV playback,
    // then call StopAudition() to clear audition state if active.
    CloseWav();
    StopAudition();
    // Debug: report playback state after attempting stop
    if (WaveX::Comm::s_hw) {
        bool wav_playing = WaveX::AudioEngine::IsWavPlaying();
        WaveX::Comm::s_hw->PrintLine("DAISY: After stop - IsWavPlaying=%d", wav_playing ? 1 : 0);
    }

    // Send sample stop response
    SampleStopRespMessage stop_resp;
    stop_resp.success = 1;  // Successfully stopped
    stop_resp.reserved[0] = 0;
    stop_resp.reserved[1] = 0;
    stop_resp.reserved[2] = 0;

    WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_SAMPLE_STOP_RESP, &stop_resp, sizeof(stop_resp));

    if (WaveX::Comm::s_hw) {
        WaveX::Comm::s_hw->PrintLine("DAISY: Sample stop response sent");
    }
}

// Process sample play index request (existing function)
void ProcessSamplePlayIndexRequest(uint32_t file_index) {
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    if (WaveX::Comm::s_hw) {
        WaveX::Comm::s_hw->PrintLine("DAISY: ProcessSamplePlayIndexRequest called with index: %lu",
                                     (unsigned long)file_index);
    }

    // Get file path for index
    char file_path[200] = {0};
    if (FileSystem::GetFilePathByIndex(file_index, file_path, sizeof(file_path))) {
        if (WaveX::Comm::s_hw) {
            WaveX::Comm::s_hw->PrintLine("DAISY: Playing sample at path: '%s'", file_path);
        }

        // Call the regular play request function
        ProcessSamplePlayRequest(file_path);
    } else {
        if (WaveX::Comm::s_hw) {
            WaveX::Comm::s_hw->PrintLine("DAISY: Failed to get file path for index %lu",
                                         (unsigned long)file_index);
        }
    }
}

// Process sample get path request (existing function)
void ProcessSampleGetPathRequest(uint32_t file_index) {
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    if (WaveX::Comm::s_hw) {
        WaveX::Comm::s_hw->PrintLine("DAISY: ProcessSampleGetPathRequest called with index: %lu",
                                     (unsigned long)file_index);
    }

    // Get file path for index
    char file_path[200] = {0};
    if (FileSystem::GetFilePathByIndex(file_index, file_path, sizeof(file_path))) {
        SamplePathResponseMessage response;
        response.index = file_index;
        strncpy(response.path, file_path, sizeof(response.path) - 1);
        response.path[sizeof(response.path) - 1] = '\0';

        WaveX::Comm::UartLinkSend(
            WaveX::Protocol::MSG_SAMPLE_GET_PATH_RESP, &response, sizeof(response));

        if (WaveX::Comm::s_hw) {
            WaveX::Comm::s_hw->PrintLine("DAISY: Sent file path response: index=%lu path='%s'",
                                         (unsigned long)file_index,
                                         file_path);
        }
    } else {
        if (WaveX::Comm::s_hw) {
            WaveX::Comm::s_hw->PrintLine("DAISY: Failed to get file path for index %lu",
                                         (unsigned long)file_index);
        }
    }
}

}  // namespace Comm
}  // namespace WaveX

#endif  // WAVEX_INTER_MCU_LINK_ENABLED
