#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../../shared/spi_protocol/protocol.h" // For PKT_SIZE_* constants

// Constants for packet sizes
#define CMD_PKT_SIZE PKT_SIZE_32
#define DATA_PKT_SIZE PKT_SIZE_1024  // Increased from 256 to 1024 bytes
#define MAX_PKT_SIZE 2048  // Support up to 2KB packets

namespace WaveX {
namespace Comm {

// Filesystem operation functions
void ProcessBrowseRequest(const char* path, size_t start_index, uint8_t max_entries = 20);
void ProcessSamplePlayRequest(const char* file_path);
void ProcessSampleStopRequest();
void ProcessSamplePlayIndexRequest(uint32_t file_index);
void ProcessSampleGetPathRequest(uint32_t file_index);

} // namespace Comm
} // namespace WaveX
