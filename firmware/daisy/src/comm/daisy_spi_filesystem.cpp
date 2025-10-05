#include "daisy_spi_link.h"

#if WAVEX_SPI_LINK_ENABLED

// Force platform define for linter
#ifndef DAISY_PLATFORM
#define DAISY_PLATFORM 1
#endif

#include <string.h>
#include <stdint.h>
#include "daisy_seed.h"

#include "config/logging_config.h" // For logging macros
#include "spi_protocol/protocol.h" // For WaveX::Protocol namespace
#include "../storage/fs_browse.h" // For file browsing
#include "../audio/audio_engine.h" // For sample audition
#include "daisy_spi_message_handlers.h"
#include "daisy_spi_filesystem.h"

using namespace daisy;
using namespace WaveX::Protocol;

// Forward declaration for s_hw (defined in main file)
extern daisy::DaisySeed* s_hw;

// Forward declaration for Spi_SendPreCreatedPacket (defined in main file)
extern bool Spi_SendPreCreatedPacket(const uint8_t* packet_data, size_t packet_size);

// FileSystem class declaration
namespace WaveX {
namespace Storage {
class FileSystem {
public:
    static bool GetFilePathByIndex(uint32_t file_index, char* file_path, size_t max_len);
};
} // namespace Storage
} // namespace WaveX

// Directory state for file browsing
static char s_current_directory[96] = "/";
static WaveX::Storage::FileEntry s_current_file_entries[50]; // Increased to accommodate more files
static size_t s_current_file_count = 0;
static bool s_directory_state_valid = false;

// ============================================================================
// FileSystem Implementation
// ============================================================================

// Implementation of FileSystem::GetFilePathByIndex
namespace WaveX {
namespace Storage {
bool FileSystem::GetFilePathByIndex(uint32_t file_index, char* file_path, size_t max_len) {
    // Use cached directory state to get file path by index
    if (!s_directory_state_valid || file_index >= s_current_file_count) {
        return false; // No valid directory state or index out of range
    }
    
    const WaveX::Storage::FileEntry& entry = s_current_file_entries[file_index];
    
    // Build full path: current_directory + "/" + filename
    size_t dir_len = strlen(s_current_directory);
    size_t name_len = strlen(entry.name);
    
    // Check if we have enough space
    if (dir_len + 1 + name_len + 1 > max_len) {
        return false; // Path too long
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
} // namespace Storage
} // namespace WaveX

// ============================================================================
// Filesystem Operations
// ============================================================================

namespace WaveX {
namespace Comm {

// Process browse request (existing function - updated for new format)
void ProcessBrowseRequest(const char* path, size_t start_index, uint8_t max_entries)
{
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "IN MSG BROWSE_REQ path=%s start_index=%u max_entries=%u", 
                           path, (uint32_t)start_index, max_entries);

    // Allocate buffer for file entries
    FileEntry entries[50]; // Max 50 entries per response (matches cache size)
    size_t actual_max_entries = (max_entries > 50) ? 50 : max_entries;
    
    size_t total_count = 0;
    size_t entries_written = 0;
    
    // Get directory listing from FatFS
    bool success = ListDir(path, entries, actual_max_entries, total_count, start_index, entries_written);
    
    if (!success) {
        WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "Failed to list directory: %s", path);
        return;
    }
    
    WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "Directory listing: total=%u written=%u", (uint32_t)total_count, (uint32_t)entries_written);
    
    // Cache the directory state for index-based lookups
    strncpy(s_current_directory, path, sizeof(s_current_directory) - 1);
    s_current_directory[sizeof(s_current_directory) - 1] = '\0';
    
    // Cache all entries (not just the paginated ones)
    if (start_index == 0) {
        // If this is the first page, cache all entries
        size_t all_entries_count = 0;
        FileEntry all_entries[50];
        ListDir(path, all_entries, 50, total_count, 0, all_entries_count);
        
        // Ensure we don't exceed our cache size
        s_current_file_count = (all_entries_count > 50) ? 50 : all_entries_count;
        for (size_t i = 0; i < s_current_file_count; i++) {
            s_current_file_entries[i] = all_entries[i];
        }
        s_directory_state_valid = true;
        
        if (s_hw) {
            s_hw->PrintLine("DAISY: Cached directory state: %u entries from '%s'", (uint32_t)s_current_file_count, path);
        }
    }
    
    // Convert FileEntry to FileEntryWire for transmission
    FileEntryWire wire_entries[50];
    for (size_t i = 0; i < entries_written && i < 50; i++) {
        wire_entries[i].is_dir = entries[i].is_dir;
        wire_entries[i].size_bytes = entries[i].size_bytes;
        strncpy(wire_entries[i].name, entries[i].name, sizeof(wire_entries[i].name) - 1);
        wire_entries[i].name[sizeof(wire_entries[i].name) - 1] = '\0';
    }
    
    // Create browse response packet using the protocol handler
    uint8_t response_buffer[MAX_PKT_SIZE];
    size_t pkt_size = ProtocolHandler::CreateBrowseRespPacket(response_buffer, sizeof(response_buffer),
                                                           total_count, wire_entries, entries_written);
    
    if (pkt_size > 0) {
        // Send the response
        if (s_hw) {
            s_hw->PrintLine("DAISY: Sending browse response: total=%u start=%u count=%u", 
                           (uint32_t)total_count, (uint32_t)start_index, (uint32_t)entries_written);
        }
        
        Spi_SendPreCreatedPacket(response_buffer, pkt_size);
    } else {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Failed to create browse response packet");
        }
    }
}

// Process sample play request (existing function)
void ProcessSamplePlayRequest(const char* file_path)
{
    using namespace WaveX::Protocol;
    using namespace WaveX::AudioEngine;

    if (s_hw) {
        s_hw->PrintLine("DAISY: ProcessSamplePlayRequest called with path: '%s'", file_path);
    }

    // Stop any current playback first
    CloseWav();

    // Start playback using the existing WAV playback system
    if (!OpenWav(file_path)) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Failed to open WAV file for playback: '%s'", file_path);
        }
        
        // Send error response
        ErrorMessage error;
        error.code = 1; // File open error
        strncpy(error.msg, "Failed to open WAV file", sizeof(error.msg) - 1);
        error.msg[sizeof(error.msg) - 1] = '\0';
        
        uint8_t response_buffer[MAX_PKT_SIZE];
        size_t pkt_size = ProtocolHandler::CreateErrorPacket(response_buffer, sizeof(response_buffer), error);
        Spi_SendPreCreatedPacket(response_buffer, pkt_size);
        return;
    }

    // Send success acknowledgment
    AckMessage ack;
    ack.serial_id = 0; // TODO: Get actual serial ID
    
    uint8_t response_buffer[MAX_PKT_SIZE];
    size_t pkt_size = ProtocolHandler::CreateAckPacket(response_buffer, sizeof(response_buffer), ack);
    Spi_SendPreCreatedPacket(response_buffer, pkt_size);
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Sample playback started successfully for: '%s'", file_path);
    }
}

// Process sample stop request (existing function)
void ProcessSampleStopRequest(uint8_t slot)
{
    using namespace WaveX::Protocol;
    using namespace WaveX::AudioEngine;

    if (s_hw) {
        s_hw->PrintLine("DAISY: ProcessSampleStopRequest called (slot=%u)", (unsigned)slot);
    }

    // Stop current playback. Call CloseWav() to stop any WAV playback,
    // then call StopAudition() to clear audition state if active.
    CloseWav();
    StopAudition();
    // Debug: report playback state after attempting stop
    if (s_hw) {
        bool wav_playing = WaveX::AudioEngine::IsWavPlaying();
        s_hw->PrintLine("DAISY: After stop - IsWavPlaying=%d", wav_playing ? 1 : 0);
    }

    // Send sample stop response
    SampleStopRespMessage stop_resp;
    stop_resp.success = 1; // Successfully stopped
    stop_resp.reserved[0] = 0;
    stop_resp.reserved[1] = 0;
    stop_resp.reserved[2] = 0;
    
    uint8_t response_buffer[MAX_PKT_SIZE];
    size_t pkt_size = ProtocolHandler::CreateSampleStopRespPacket(response_buffer, sizeof(response_buffer), stop_resp);
    Spi_SendPreCreatedPacket(response_buffer, pkt_size);
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Sample stop response sent");
    }
}

// Process sample play index request (existing function)
void ProcessSamplePlayIndexRequest(uint32_t file_index)
{
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    if (s_hw) {
        s_hw->PrintLine("DAISY: ProcessSamplePlayIndexRequest called with index: %lu", (unsigned long)file_index);
    }

    // Get file path for index
    char file_path[200] = {0};
    if (FileSystem::GetFilePathByIndex(file_index, file_path, sizeof(file_path))) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Playing sample at path: '%s'", file_path);
        }
        
        // Call the regular play request function
        ProcessSamplePlayRequest(file_path);
    } else {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Failed to get file path for index %lu", (unsigned long)file_index);
        }
    }
}

// Process sample get path request (existing function)
void ProcessSampleGetPathRequest(uint32_t file_index)
{
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    if (s_hw) {
        s_hw->PrintLine("DAISY: ProcessSampleGetPathRequest called with index: %lu", (unsigned long)file_index);
    }

    // Get file path for index
    char file_path[200] = {0};
    if (FileSystem::GetFilePathByIndex(file_index, file_path, sizeof(file_path))) {
        SamplePathResponseMessage response;
        response.index = file_index;
        strncpy(response.path, file_path, sizeof(response.path) - 1);
        response.path[sizeof(response.path) - 1] = '\0';
        
        uint8_t response_buffer[MAX_PKT_SIZE];
        size_t pkt_size = ProtocolHandler::CreateSamplePathResponsePacket(response_buffer, sizeof(response_buffer), response);
        Spi_SendPreCreatedPacket(response_buffer, pkt_size);
        
        if (s_hw) {
            s_hw->PrintLine("DAISY: Sent file path response: index=%lu path='%s'", 
                           (unsigned long)file_index, file_path);
        }
    } else {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Failed to get file path for index %lu", (unsigned long)file_index);
        }
    }
}

} // namespace Comm
} // namespace WaveX

#endif // WAVEX_SPI_LINK_ENABLED
