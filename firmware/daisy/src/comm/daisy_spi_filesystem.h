#pragma once

#include <stdint.h>
#include <stddef.h>
#include "spi_protocol/protocol.h" // For PKT_SIZE_* constants

namespace WaveX {
namespace Comm {

// Filesystem operation functions
void ProcessBrowseRequest(const char* path, size_t start_index, uint8_t max_entries = 20);
void ProcessSamplePlayRequest(const char* file_path);
void ProcessSampleStopRequest(uint8_t slot);
void ProcessSamplePlayIndexRequest(uint32_t file_index);
void ProcessSampleGetPathRequest(uint32_t file_index);

} // namespace Comm
} // namespace WaveX
